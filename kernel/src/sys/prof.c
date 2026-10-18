/* Sampling profiler, enabled by the "prof" kernel command line word. Every
   timer tick on every CPU records where that CPU was: the user instruction
   of the task it ran, the kernel instruction, or idle. A kernel thread
   prints the hottest addresses with their task names every 10 seconds and
   clears the counts; addresses are resolved against the unstripped ELFs on
   the build host. AP syscalls that had to retire to the BSP are counted by
   number, because each one is a migration through the BSP. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "arch/pstate.h"
#include "mm/heap.h"

#define PROF_SLOTS (1u << 16)
#define PROF_TOP 400
#define PROF_SYSCALLS 512
#define PROF_PERIOD_MS 10000

struct prof_slot {
    uint64_t key;   /* rip, 0 = empty */
    int pid;        /* task pid, -1 = kernel/idle */
    uint32_t count;
};

static struct prof_slot *slots;
static bool enabled;
static uint32_t cpu_user[CPU_MAX_COUNT], cpu_kernel[CPU_MAX_COUNT], cpu_idle[CPU_MAX_COUNT];
static uint32_t syscall_retire[PROF_SYSCALLS], quantum_retire, lost;

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

void prof_tick(const struct regs *r, bool idle) {
    if (!enabled) return;
    unsigned cpu = cpu_current_index();
    if (cpu >= CPU_MAX_COUNT) return;
    struct task *t = current_task;
    if ((r->cs & 3) == 3 && t) {
        __atomic_add_fetch(&cpu_user[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip, t->pid);
    } else if (idle || !t) {
        __atomic_add_fetch(&cpu_idle[cpu], 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&cpu_kernel[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip, -1);
    }
}

void prof_ap_retire(uint64_t vector, uint64_t number) {
    if (!enabled) return;
    if (vector == 0x80) __atomic_add_fetch(&syscall_retire[number < PROF_SYSCALLS ? number : PROF_SYSCALLS - 1], 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&quantum_retire, 1, __ATOMIC_RELAXED);
}

static const char *task_name(int pid, bool *thread) {
    *thread = false;
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid) { *thread = t->is_thread; return t->name; }
    return "?";
}

static void dump(void) {
    uint64_t total = 0;
    kprintf("prof: ---- %u ms window ----\n", PROF_PERIOD_MS);
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
    for (unsigned n = 0; n < PROF_TOP; n++) {
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
    for (unsigned i = 0; i < PROF_SYSCALLS; i++) {
        uint32_t c = __atomic_exchange_n(&syscall_retire[i], 0, __ATOMIC_RELAXED);
        if (c >= 100) kprintf("prof: syscall %u retired to BSP %u times\n", i, c);
    }
    kprintf("prof: quantum/irq retires %u, lost samples %u, busy samples %llu\n",
            __atomic_exchange_n(&quantum_retire, 0, __ATOMIC_RELAXED),
            __atomic_exchange_n(&lost, 0, __ATOMIC_RELAXED), (unsigned long long)total);
    for (uint32_t i = 0; i < PROF_SLOTS; i++) {
        slots[i].count = 0;
        slots[i].pid = 0;
        __atomic_store_n(&slots[i].key, 0, __ATOMIC_RELEASE);
    }
}

static void prof_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep_ms(PROF_PERIOD_MS);
        /* Stop sampling while the table is read and cleared. */
        enabled = false;
        dump();
        enabled = true;
    }
}

void prof_init(void) {
    if (!cmdline_has("prof")) return;
    slots = kzalloc(PROF_SLOTS * sizeof *slots);
    if (!slots) return;
    enabled = true;
    kthread_create("prof", prof_thread, NULL);
    kprintf("prof: sampling every timer tick on every CPU\n");
}
