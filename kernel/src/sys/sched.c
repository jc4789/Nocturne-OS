/* The scheduler. Each CPU has a run queue, which kei (sys/kei.c) orders by virtual run time, and an
   idle task; each CPU switches among the tasks its queue holds by itself, takes waiting tasks from
   a busier queue when its own runs dry, and a woken task goes to an idle CPU first.

   A queue's lock guards its kei records and the state of the tasks it holds. The CPU that switches
   holds its queue's lock from the decision until it runs on the next task's stack
   (sched_post_switch): no other CPU can wake, take or free the task it left before that task's
   context is saved, so the task is quiesced for kei as soon as the decision is made. Two queues'
   locks are taken in CPU order. task_lock comes before a wait queue's lock, and that before a run
   queue's. Every one of them is held with interrupts off. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/cpuset.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/object.h"
#include "sys/channel.h"
#include "dev/pciuser.h"
#include "sys/proc.h"
#include "gui/wm.h"
#include "net/net.h"
#include "abi.h"

struct task *task_list;
struct spinlock task_lock;

/* The public app/thread IDs are signed 32-bit. Never wrap or reuse an ID:
   queued channel credentials remain meaningful after their sender ends. */
static uint32_t next_pid;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void kthread_trampoline(void);

#define BALANCE_NS (4 * NS_PER_MS) /* how often a CPU with tasks waiting looks for a much idler one */
#define STEAL_PROBES 8            /* waiting tasks one look at another queue examines */
#define KEI_SWITCH_COST_NS 10000  /* a switch (XSAVE, CR3, the caches' recovery), estimated */
#define KEI_OVERHEAD_PPM 10000    /* aim at 1% lost to switching: the retention is about 99 times it */
#define KILLED_NAP_NS (10 * NS_PER_MS) /* a killed task's waits end this soon, whether a waker comes or not */

PERCPU static struct cpu_queue {
    struct spinlock lock;
    struct kei_rq rq;
    struct task *idle;
    struct task *evicted;   /* sched_cpu_leave: the task this CPU left, for another queue */
    uint64_t stamp;         /* when rq.current (or the idle task) was last charged (clock_now_ns) */
    uint64_t balanced_at;   /* its last look for an idler queue (rebalance) */
    /* While tasks wait behind the one that runs: when kei would let one of them run instead
       (slice_arm). A stale one finds nothing to do. */
    struct alarm slice;
    size_t steal_cursor;
    unsigned cpu;
    bool prepared, leaving;
} __attribute__((aligned(64))) queue;

PERCPU struct task *sched_task;
PERCPU volatile bool sched_resched;

/* Every queue's heap holds every task there is, so none ever fills. capacity_lock comes before a
   queue's lock. */
static struct spinlock capacity_lock;
static size_t queue_capacity, task_count;
static uint64_t kei_retention_v;
static struct sched_stats stats;
#define KEI_OWNER(k) ((struct task *)((char *)(k) - __builtin_offsetof(struct task, kei)))
#define STAT(f) __atomic_fetch_add(&stats.f, 1, __ATOMIC_RELAXED)

/* Ended tasks no one else collects: the reaper frees them once they are off their CPUs. */
static struct task *dead;
static struct wait_queue dead_wait;

static bool wq_take_blocked(struct task *t, bool timed);
static void wake_fire(struct alarm *a, struct regs *r);
static void slice_fire(struct alarm *a, struct regs *r);

/* ---- time ---- */



/* q's lock held: what ran on its CPU since the last charge, interrupts and kernel time included. */
static void charge(struct cpu_queue *q) {
    uint64_t now = clock_now_ns();
    if (now <= q->stamp) return;
    uint64_t ns = now - q->stamp;
    q->stamp = now;
    if (q->rq.current) {
        if (kei_charge(&q->rq, ns) == KEI_OK) KEI_OWNER(q->rq.current)->cpu_ns += ns;
    } else {
        q->idle->cpu_ns += ns;
    }
}

/* ---- queues ---- */

static struct cpu_queue *this_queue(void) { return this_cpu_ptr(queue); }

static void lock2(unsigned a, unsigned b) {
    spin_take(&per_cpu(queue, a < b ? a : b).lock);
    if (a != b) spin_take(&per_cpu(queue, a < b ? b : a).lock);
}
static void unlock2(unsigned a, unsigned b) {
    spin_give(&per_cpu(queue, a).lock);
    if (a != b) spin_give(&per_cpu(queue, b).lock);
}

/* Hints, read without the lock: the decisions are made again under it. */
static size_t waiting(unsigned cpu) { return __atomic_load_n(&per_cpu(queue, cpu).rq.count, __ATOMIC_RELAXED); }
static size_t resident(unsigned cpu) {
    return waiting(cpu) + (__atomic_load_n(&per_cpu(queue, cpu).rq.current, __ATOMIC_RELAXED) != NULL);
}
static bool usable(unsigned cpu) {
    return __atomic_load_n(&per_cpu(queue, cpu).prepared, __ATOMIC_ACQUIRE) && cpu_online_at(cpu) &&
           !__atomic_load_n(&per_cpu(queue, cpu).leaving, __ATOMIC_ACQUIRE);
}
bool sched_cpu_idle(unsigned cpu) {
    return cpu < topology.cpus && __atomic_load_n(&per_cpu(queue, cpu).prepared, __ATOMIC_ACQUIRE) && !resident(cpu);
}
bool sched_cpu_empty(unsigned cpu) {
    return !resident(cpu) && !__atomic_load_n(&per_cpu(queue, cpu).evicted, __ATOMIC_ACQUIRE);
}

/* Every prepared queue's heap holds at least need tasks. False without memory. */
static bool capacity_reserve(size_t need) {
    for (;;) {
        uint64_t f = spin_lock(&capacity_lock);
        size_t cap = queue_capacity;
        spin_unlock(&capacity_lock, f);
        if (need <= cap) return true;
        size_t want = cap ? cap * 2 : 64;
        while (want < need) want *= 2;
        /* each prepared queue's new heap, then its old one */
        struct kei_task ***heaps = kzalloc(2 * (size_t)topology.cpus * sizeof *heaps);
        if (!heaps) return false;
        struct kei_task ***old = heaps + topology.cpus;
        bool ok = true;
        for (unsigned cpu = 0; cpu < topology.cpus && ok; cpu++)
            if (per_cpu(queue, cpu).prepared && !(heaps[cpu] = kzalloc(want * sizeof **heaps))) ok = false;
        f = spin_lock(&capacity_lock);
        if (ok && queue_capacity < want) {
            for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
                struct cpu_queue *q = &per_cpu(queue, cpu);
                if (!q->prepared || !heaps[cpu]) continue;
                spin_take(&q->lock);
                old[cpu] = q->rq.heap;
                if (kei_rq_resize(&q->rq, heaps[cpu], want) != KEI_OK) panic("kei: queue %u resize failed", cpu);
                spin_give(&q->lock);
                heaps[cpu] = NULL;
            }
            queue_capacity = want;
        }
        spin_unlock(&capacity_lock, f);
        for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
            kfree(heaps[cpu]);
            kfree(old[cpu]);
        }
        kfree(heaps);
        if (!ok) return false;
    }
}

/* A queue with an idle task, at the capacity every queue has. */
static bool queue_prepare(unsigned cpu, struct task *idle) {
    struct cpu_queue *q = &per_cpu(queue, cpu);
    for (;;) {
        uint64_t f = spin_lock(&capacity_lock);
        size_t cap = queue_capacity ? queue_capacity : 64;
        spin_unlock(&capacity_lock, f);
        struct kei_task **heap = kzalloc(cap * sizeof *heap);
        if (!heap) return false;
        f = spin_lock(&capacity_lock);
        if (cap != (queue_capacity ? queue_capacity : 64)) { /* it grew meanwhile */
            spin_unlock(&capacity_lock, f);
            kfree(heap);
            continue;
        }
        if (kei_rq_init(&q->rq, cpu, heap, cap, kei_retention_v, 0) != KEI_OK) panic("kei: queue %u setup failed", cpu);
        queue_capacity = cap;
        q->idle = idle;
        q->cpu = cpu;
        idle->cpu = cpu;
        q->stamp = clock_now_ns();
        alarm_init(&q->slice, slice_fire);
        __atomic_store_n(&q->prepared, true, __ATOMIC_RELEASE);
        spin_unlock(&capacity_lock, f);
        return true;
    }
}

/* A task's record and its extended state's area after it, in one allocation (kfree frees both). */
static struct task *task_record(void) {
    struct task *t = kzalloc(sizeof *t + fpu_storage_bytes());
    if (!t) return NULL;
    t->fpu = fpu_state_in(t + 1);
    fpu_state_init(t->fpu);
    return t;
}

static struct task *idle_task_new(const char *name) {
    struct task *t = task_record();
    if (!t) return NULL;
    t->handles = t->initial_handles;
    t->handle_capacity = INITIAL_HANDLES;
    t->is_idle = true;
    t->state = TASK_RUNNING;
    t->pml4 = kernel_pml4;
    alarm_init(&t->wake_alarm, wake_fire);
    strlcpy(t->name, name, sizeof t->name);
    uint64_t f = spin_lock(&task_lock);
    t->pid = next_pid++;
    t->all_next = task_list;
    task_list = t;
    spin_unlock(&task_lock, f);
    return t;
}

bool sched_cpu_prepare(unsigned cpu) {
    if (cpu >= topology.cpus) return false;
    if (per_cpu(queue, cpu).prepared) return true;
    char name[16];
    ksnprintf(name, sizeof name, "idle%u", cpu);
    struct task *idle = idle_task_new(name);
    return idle && queue_prepare(cpu, idle);
}

/* The CPU other than self with the most waiting tasks, at least at_least of them; self when none. */
static unsigned busiest(unsigned self, size_t at_least) {
    unsigned best = self;
    size_t most = at_least ? at_least - 1 : 0;
    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
        if (cpu == self || !__atomic_load_n(&per_cpu(queue, cpu).prepared, __ATOMIC_ACQUIRE)) continue;
        size_t n = waiting(cpu);
        if (n > most) { most = n; best = cpu; }
    }
    return best;
}

/* Interrupts off: one waiting task of src's moves to dst's queue (its lead kept). */
static bool take_from(unsigned src, unsigned dst) {
    struct cpu_queue *s = &per_cpu(queue, src), *d = &per_cpu(queue, dst);
    bool moved = false;
    lock2(src, dst);
    if (!d->leaving && d->rq.online) {
        struct kei_task *k = kei_find_stealable(&s->rq, dst, STEAL_PROBES, &d->steal_cursor);
        if (k && kei_migrate(&s->rq, &d->rq, k) == KEI_OK) {
            KEI_OWNER(k)->cpu = dst;
            moved = true;
        }
    }
    unlock2(src, dst);
    return moved;
}

/* Interrupts off, on a CPU with nothing to run: take a task that waits elsewhere. */
static bool steal(unsigned self) {
    if (!usable(self)) return false;
    unsigned src = busiest(self, 1);
    if (src == self || !take_from(src, self)) return false;
    STAT(steals);
    return true;
}



/* Where a waking task goes: its last CPU if that has nothing to run, else one that has nothing,
   else the queue with the fewest tasks (its last on a tie). */
static unsigned choose(struct task *t) {
    const struct kei_mask *allowed = t->kei.allowed;
    unsigned home = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE), best = home;
    bool home_ok = usable(home) && kei_mask_test(allowed, home);
    if (home_ok && !resident(home)) return home;
    size_t fewest = home_ok ? resident(home) : SIZE_MAX;
    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
        if (cpu == home || !usable(cpu) || !kei_mask_test(allowed, cpu)) continue;
        size_t n = resident(cpu);
        if (n < fewest) { fewest = n; best = cpu; }
    }
    return fewest == SIZE_MAX ? 0 : best;
}

/* q's lock held, interrupts off: q's slice alarm, while tasks wait behind the one that runs, for
   the moment kei would let one of them run instead. Armed from another CPU (a waker's), it kicks
   q's CPU when it comes. */
static void slice_arm(struct cpu_queue *q) {
    if (!q->rq.current || !q->rq.count) return;
    struct kei_decision d = kei_decide(&q->rq);
    if (d.switch_needed || d.service_until_recheck_ns == KEI_NO_TIMER) return;
    alarm_set(&q->slice, clock_after(d.service_until_recheck_ns));
}

/* Interrupts off, at most every BALANCE_NS on a CPU with tasks waiting: one of them to the queue
   with the fewest, when that has two fewer, whose CPU is told to look. */
static void rebalance(unsigned self) {
    struct cpu_queue *q = &per_cpu(queue, self);
    uint64_t now = clock_now_ns();
    if (now - q->balanced_at < BALANCE_NS || !usable(self)) return;
    q->balanced_at = now;
    unsigned to = self;
    size_t fewest = resident(self);
    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
        if (cpu == self || !usable(cpu)) continue;
        size_t n = resident(cpu);
        if (n + 2 <= fewest) { fewest = n; to = cpu; }
    }
    if (to != self && take_from(self, to)) {
        STAT(balances);
        cpu_kick(to);
    }
}

static void slice_fire(struct alarm *a, struct regs *r) {
    struct cpu_queue *q = (struct cpu_queue *)((char *)a - __builtin_offsetof(struct cpu_queue, slice));
    unsigned cpu = q->cpu;
    if (cpu != cpu_current_index()) {
        cpu_kick(cpu);
        return;
    }
    if (!q->prepared) return;
    spin_take(&q->lock);
    charge(q);
    bool switch_needed = q->rq.current ? kei_decide(&q->rq).switch_needed : q->rq.count != 0;
    if (!switch_needed) slice_arm(q);
    spin_give(&q->lock);
    if (switch_needed) need_resched = true;
    rebalance(cpu);
}

/* Interrupts off, no queue's lock held: t, in no queue (kei NEW or SLEEP), joins one; its CPU is
   told when t should run before what runs there. Nothing when another waker came first. */
static void enqueue(struct task *t) {
    for (;;) {
        unsigned from = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE), to = choose(t);
        lock2(from, to);
        if (t->cpu != from) { unlock2(from, to); continue; }
        if (t->kei.state != KEI_NEW && t->kei.state != KEI_SLEEP) { unlock2(from, to); return; }
        struct cpu_queue *q = &per_cpu(queue, to);
        if (q->leaving || !q->rq.online) { unlock2(from, to); continue; }
        enum kei_error e = kei_wake(&q->rq, &t->kei);
        if (e != KEI_OK) panic("kei: task %d into cpu%u failed %d", t->pid, to, e);
        __atomic_store_n(&t->cpu, to, __ATOMIC_RELEASE);
        bool kick = !q->rq.current || kei_decide(&q->rq).switch_needed;
        if (!kick) slice_arm(q);
        unlock2(from, to);
        STAT(wakes);
        if (from != to) STAT(moved_wakes);
        if (kick) {
            if (to == cpu_current_index()) need_resched = true;
            else { STAT(kicks); cpu_kick(to); }
        }
        return;
    }
}

void sched_make_ready(struct task *t) {
    if (t->is_idle) { /* the boot task, taken out of a wait: it halts until an interrupt, this one */
        __atomic_store_n(&t->state, TASK_RUNNING, __ATOMIC_RELEASE);
        cpu_kick(__atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE));
        return;
    }
    uint64_t f = irq_save();
    struct cpu_queue *q;
    for (;;) {
        unsigned cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        q = &per_cpu(queue, cpu);
        spin_take(&q->lock);
        if (t->cpu == cpu) break;
        spin_give(&q->lock);
    }
    /* Otherwise still on its CPU (it sees RUNNING when it comes to schedule), queued, or ended. */
    bool out = t->state != TASK_ZOMBIE && (t->kei.state == KEI_NEW || t->kei.state == KEI_SLEEP);
    if (t->state != TASK_ZOMBIE) __atomic_store_n(&t->state, TASK_RUNNING, __ATOMIC_RELEASE);
    spin_give(&q->lock);
    if (out) enqueue(t);
    irq_restore(f);
}

void sched_post_switch(void) {
    struct cpu_queue *q = this_queue();
    struct task *evicted = q->evicted;
    spin_give(&q->lock);
    if (evicted) {
        enqueue(evicted);
        __atomic_store_n(&q->evicted, NULL, __ATOMIC_RELEASE);
    }
}

void schedule(void) {
    uint64_t f = irq_save();
    struct cpu_queue *q = this_queue();
    struct task *prev = current_task;
    spin_take(&q->lock);
    charge(q);
    need_resched = false;
    if (!prev->is_idle) {
        /* The lock stays held until this CPU is on the next stack: no one acts on prev before. */
        if (kei_context_quiesced(&q->rq, &prev->kei) != KEI_OK) panic("kei: task %d not current", prev->pid);
        if (prev->state == TASK_ZOMBIE) kei_destroy(&q->rq, &prev->kei);
        else if (prev->state == TASK_BLOCKED) kei_sleep(&q->rq, &prev->kei);
        else if (q->leaving) {
            kei_sleep(&q->rq, &prev->kei);
            q->evicted = prev; /* another queue takes it once this CPU is off its stack */
        }
    }
    struct kei_decision d = kei_decide(&q->rq);
    struct task *next = q->idle;
    if (d.next) {
        if (kei_commit(&q->rq, d.next) != KEI_OK) panic("kei: commit on cpu%u failed", cpu_current_index());
        next = KEI_OWNER(d.next);
    }
    slice_arm(q);
    if (next == prev) {
        spin_give(&q->lock);
        irq_restore(f);
        return;
    }
    STAT(switches);
    sched_set_current(next);
    if (next->kstack) tss_set_rsp0((uint64_t)next->kstack + KSTACK_PAGES * PAGE_SIZE);
    vmm_switch(next->pml4);
    wrmsr(0xC0000100, next->tls_base);
    fpu_save(prev->fpu);
    fpu_restore(next->fpu);
    context_switch(&prev->ksp, next->ksp);
    /* prev again, on whichever CPU took it: sched_post_switch gave that CPU's lock back */
    irq_restore(f);
}

void sched_yield(void) {
    uint64_t f = irq_save();
    struct cpu_queue *q = this_queue();
    spin_take(&q->lock);
    charge(q);
    if (q->rq.current) kei_yield(&q->rq);
    spin_give(&q->lock);
    STAT(yields);
    schedule();
    irq_restore(f);
}



static void resched_irq(struct regs *r) {
    (void)r;
    need_resched = true;
}

uint64_t sched_task_cpu_ns(struct task *t) {
    uint64_t f = irq_save();
    for (;;) {
        unsigned cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        struct cpu_queue *q = &per_cpu(queue, cpu);
        spin_take(&q->lock);
        if (t->cpu != cpu) {
            spin_give(&q->lock);
            continue;
        }
        if (t->is_idle ? q->idle == t && !q->rq.current : q->rq.current == &t->kei) charge(q);
        uint64_t ns = t->cpu_ns;
        spin_give(&q->lock);
        irq_restore(f);
        return ns;
    }
}

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatch returns with interrupts on; from here to iretq they stay off. */
    cli();
    for (;;) {
        if (current_task->killed) task_exit(current_task->kill_code);
        if (!need_resched) return;
        schedule();
    }
}

/* ---- each CPU's idle task ---- */

NORETURN void sched_idle(void) {
    unsigned self = cpu_current_index();
    for (;;) {
        cli();
        cpu_park_check();
        if (cpu_jobs_poll()) continue;
        if (!resident(self)) steal(self);
        if (need_resched || resident(self)) {
            schedule();
            continue;
        }
        /* A kick between the look and hlt is held pending, and ends the hlt; this CPU's alarms
           (sys/alarm.c) end it when one is due. */
        __asm__ volatile("sti; hlt" : : : "memory");
    }
}

NORETURN void sched_cpu_start(void) {
    struct cpu_queue *q = this_queue();
    sched_set_current(q->idle);
    spin_take(&q->lock);
    q->stamp = clock_now_ns();
    __atomic_store_n(&q->leaving, false, __ATOMIC_RELEASE);
    spin_give(&q->lock);
    sched_idle();
}

void sched_cpu_leave(unsigned cpu) {
    if (!cpu || cpu >= topology.cpus || !per_cpu(queue, cpu).prepared) return;
    uint64_t f = irq_save();
    lock2(0, cpu);
    struct cpu_queue *q = &per_cpu(queue, cpu);
    __atomic_store_n(&q->leaving, true, __ATOMIC_RELEASE);
    while (q->rq.count) {
        struct kei_task *k = q->rq.heap[0];
        if (kei_migrate(&q->rq, &per_cpu(queue, 0).rq, k) != KEI_OK) panic("kei: cpu%u could not give its tasks back", cpu);
        __atomic_store_n(&KEI_OWNER(k)->cpu, 0, __ATOMIC_RELEASE);
    }
    unlock2(0, cpu);
    irq_restore(f);
    cpu_kick(cpu); /* the task it runs goes too (schedule), then it parks (cpu_park_check) */
}

void sched_get_stats(struct sched_stats *out) {
    out->switches = __atomic_load_n(&stats.switches, __ATOMIC_RELAXED);
    out->wakes = __atomic_load_n(&stats.wakes, __ATOMIC_RELAXED);
    out->moved_wakes = __atomic_load_n(&stats.moved_wakes, __ATOMIC_RELAXED);
    out->steals = __atomic_load_n(&stats.steals, __ATOMIC_RELAXED);
    out->balances = __atomic_load_n(&stats.balances, __ATOMIC_RELAXED);
    out->kicks = __atomic_load_n(&stats.kicks, __ATOMIC_RELAXED);
    out->yields = __atomic_load_n(&stats.yields, __ATOMIC_RELAXED);
}

/* ---- task lifecycle ---- */

/* An ended task's stacks and record go only once its CPU has left them: its kei record is dead,
   under its queue's lock, only after the switch away (schedule). A task never started is off. */
static void off_cpu_wait(struct task *t) {
    for (;;) {
        uint64_t f = irq_save();
        unsigned cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        spin_take(&per_cpu(queue, cpu).lock);
        bool off = t->kei.state == KEI_DEAD || t->kei.state == KEI_NEW;
        spin_give(&per_cpu(queue, cpu).lock);
        irq_restore(f);
        if (off) return;
        pause();
    }
}

/* task_lock held: t leaves the list; it is the caller's to free, and no one else's. */
static void task_unlink_locked(struct task *t) {
    struct task **pp = &task_list;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    t->all_next = NULL;
    task_count--;
}

/* An unlinked task: its kernel stack, a thread's user stack, and its record. */
static void task_free(struct task *t) {
    off_cpu_wait(t);
    net_task_exit(t);
    if (t->is_thread && t->user_stack && t->pml4 != kernel_pml4) {
        vmm_user_free(t->pml4, t->user_stack, t->user_stack_size);
        /* Both guard pages belong to the stack's reservation. */
        if (t->process && t->process != t && t->process->pml4 == t->pml4)
            mmap_hole_add(t->process, t->user_stack - PAGE_SIZE, t->user_stack_size + 2 * PAGE_SIZE);
    }
    vfree(t->kstack, KSTACK_PAGES);
    handle_table_free(t);
    kfree((void *)t->allowed.words);
    kfree(t);
}

void task_discard(struct task *t) {
    uint64_t f = spin_lock(&task_lock);
    task_unlink_locked(t);
    spin_unlock(&task_lock, f);
    task_free(t);
}

/* task_lock held: t, ended, goes to the reaper. */
static void reap_locked(struct task *t) {
    task_unlink_locked(t);
    spin_take(&dead_wait.lock);
    t->dead_next = dead;
    dead = t;
    spin_give(&dead_wait.lock);
    wq_wake_all(&dead_wait);
}

static void reaper(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock(&dead_wait.lock);
        while (!dead) {
            wq_wait_locked(&dead_wait, &dead_wait.lock);
            spin_take(&dead_wait.lock);
        }
        struct task *list = dead;
        dead = NULL;
        spin_unlock(&dead_wait.lock, f);
        while (list) {
            struct task *t = list;
            list = t->dead_next;
            task_free(t);
        }
    }
}

void sched_init(void) {
    kei_retention_v = kei_retention_from_cost(KEI_SWITCH_COST_NS, KEI_OVERHEAD_PPM, 500000, 4000000);
    struct task *boot = task_record();
    if (!boot) panic("sched: no memory");
    boot->handles = boot->initial_handles;
    boot->handle_capacity = INITIAL_HANDLES;
    boot->pid = next_pid++;
    boot->is_idle = true;
    strlcpy(boot->name, "idle0", sizeof boot->name);
    boot->state = TASK_RUNNING;
    boot->pml4 = kernel_pml4;
    alarm_init(&boot->wake_alarm, wake_fire);
    sched_set_current(boot);
    task_list = boot;
    if (!queue_prepare(0, boot)) panic("sched: no memory");
    vector_register(VEC_RESCHED, resched_irq);
    if (!kthread_create("reaper", reaper, NULL)) panic("sched: no reaper");
    kprintf("sched: kei on every CPU (retention %lluus)\n", (unsigned long long)(kei_retention_v / 1000));
}

struct task *task_alloc(const char *name) {
    struct task *t = task_record();
    if (!t) return NULL;
    t->handles = t->initial_handles;
    t->handle_capacity = INITIAL_HANDLES;
    t->kstack = vmalloc(KSTACK_PAGES);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    strlcpy(t->name, name, sizeof t->name);
    t->state = TASK_BLOCKED; /* not runnable until made ready */
    t->pml4 = kernel_pml4;
    alarm_init(&t->wake_alarm, wake_fire);
    t->start_ns = clock_now_ns();
    t->ksp = (uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE;
    t->cpu = cpu_current_index();
    uint64_t f = spin_lock(&task_lock);
    if (next_pid > INT32_MAX) {
        spin_unlock(&task_lock, f);
        vfree(t->kstack, KSTACK_PAGES);
        kfree(t);
        return NULL;
    }
    t->pid = next_pid++;
    kei_task_init(&t->kei, (uint64_t)t->pid, KEI_WEIGHT_ONE, NULL);
    t->all_next = task_list;
    task_list = t;
    size_t need = ++task_count;
    spin_unlock(&task_lock, f);
    /* Room in every queue before it can be in one. */
    if (!capacity_reserve(need)) {
        f = spin_lock(&task_lock);
        task_unlink_locked(t);
        spin_unlock(&task_lock, f);
        vfree(t->kstack, KSTACK_PAGES);
        kfree(t);
        return NULL;
    }
    return t;
}

static void push(struct task *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

#define ANY_CPU UINT32_MAX

static struct task *kthread_new(const char *name, void (*fn)(void *), void *arg, unsigned cpu) {
    struct task *t = task_alloc(name);
    if (!t) return NULL;
    if (cpu != ANY_CPU) {
        uint64_t *words = cpu < topology.cpus ? kzalloc(cpuset_words(topology.cpus) * sizeof *words) : NULL;
        if (!words) {
            task_discard(t);
            return NULL;
        }
        cpuset_add(words, cpu);
        t->allowed = (struct kei_mask){words, topology.cpus};
        t->kei.allowed = &t->allowed;
        t->cpu = cpu; /* in no queue yet: its home, which choose keeps */
    }
    push(t, (uint64_t)kthread_trampoline); /* ret target of context_switch */
    push(t, 0);                           /* rbp */
    push(t, 0);                           /* rbx */
    push(t, (uint64_t)fn);                /* r12 */
    push(t, (uint64_t)arg);               /* r13 */
    push(t, 0);                           /* r14 */
    push(t, 0);                           /* r15 */
    t->parent = NULL;
    sched_make_ready(t);
    return t;
}

struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) {
    return kthread_new(name, fn, arg, ANY_CPU);
}

struct task *kthread_create_on(const char *name, void (*fn)(void *), void *arg, unsigned cpu) {
    return kthread_new(name, fn, arg, cpu);
}

void kthread_start(void (*fn)(void *), void *arg) {
    sti();
    fn(arg);
    task_exit(0);
}

/* ---- wait queues ---- */

/* The sleeps on a deadline alone: nothing wakes this queue, only the clock (or a kill) takes a
   sleeper out of it. */
static struct wait_queue deadline_wait;

/* Interrupts off. The task sleeps on wq (NULL: on its deadline alone) until a waker or its deadline
   (clock_now_ns; CLOCK_NEVER: none) comes, unless word, read under wq's lock, has moved on from
   seen. held, a lock the caller took to decide that it sleeps (it may be wq's own), is given back
   once the task is enrolled. False when the deadline ended the sleep. */
static bool wq_sleep(struct wait_queue *wq, const uint64_t *word, uint64_t seen, struct spinlock *held,
                     uint64_t deadline) {
    struct task *t = current_task;
    if (!wq) wq = &deadline_wait;
    t->timed_out = false;
    if (held != &wq->lock) spin_take(&wq->lock);
    bool sleep = !word || __atomic_load_n(word, __ATOMIC_SEQ_CST) == seen;
    if (sleep) {
        t->wq = wq;
        t->wq_next = wq->head;
        wq->head = t;
        if (!t->is_idle) {
            __atomic_store_n(&t->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
            /* A kill after this finds it blocked and wakes it (kill_locked); one before is seen
               here, and the wait ends soon: the caller looks at killed again, or waits on. */
            if (__atomic_load_n(&t->killed, __ATOMIC_SEQ_CST)) deadline = MIN(deadline, clock_after(KILLED_NAP_NS));
        }
    }
    spin_give(&wq->lock);
    if (held && held != &wq->lock) spin_give(held);
    if (!sleep) return true;
    if (deadline != CLOCK_NEVER) alarm_set(&t->wake_alarm, deadline);
    if (t->is_idle) {
        /* An idle task (the boot task) has no stack of its own to sleep on: it stays in wq and lets
           this CPU's other tasks run, halting in between, until a waker takes it out and kicks
           this CPU (sched_make_ready), or its alarm does. */
        while (__atomic_load_n(&t->wq, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("sti; hlt; cli" : : : "memory");
            schedule();
        }
    } else {
        schedule();
    }
    if (deadline != CLOCK_NEVER) alarm_cancel(&t->wake_alarm);
    return !t->timed_out;
}

/* Interrupts off: t, blocked (or an idle task waiting), leaves its wait queue, unless a waker on
   another CPU took it out first (and wakes it), or it is not in one (a task not started yet).
   True: the caller wakes it. timed: its deadline ended the wait. */
static bool wq_take_blocked(struct task *t, bool timed) {
    struct wait_queue *wq = __atomic_load_n(&t->wq, __ATOMIC_ACQUIRE);
    if (!wq) return false;
    spin_take(&wq->lock);
    bool here = t->wq == wq && (t->is_idle || __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED);
    if (here) {
        struct task **pp = &wq->head;
        while (*pp && *pp != t) pp = &(*pp)->wq_next;
        if (*pp) *pp = t->wq_next;
        t->wq_next = NULL;
        t->timed_out = timed;
        __atomic_store_n(&t->wq, NULL, __ATOMIC_RELEASE);
    }
    spin_give(&wq->lock);
    return here;
}

/* A sleeper's deadline (wq_sleep), on the CPU it slept on. */
static void wake_fire(struct alarm *a, struct regs *r) {
    struct task *t = (struct task *)((char *)a - __builtin_offsetof(struct task, wake_alarm));
    if (wq_take_blocked(t, true)) sched_make_ready(t);
}

void wq_wait(struct wait_queue *wq) {
    uint64_t f = irq_save();
    wq_sleep(wq, NULL, 0, NULL, CLOCK_NEVER);
    irq_restore(f);
}

bool wq_wait_until(struct wait_queue *wq, uint64_t deadline) {
    uint64_t f = irq_save();
    bool ok = wq_sleep(wq, NULL, 0, NULL, deadline);
    irq_restore(f);
    return ok;
}

void wq_wait_locked(struct wait_queue *wq, struct spinlock *held) { wq_sleep(wq, NULL, 0, held, CLOCK_NEVER); }

bool wq_wait_locked_until(struct wait_queue *wq, struct spinlock *held, uint64_t deadline) {
    return wq_sleep(wq, NULL, 0, held, deadline);
}

bool wq_wait_unless(struct wait_queue *wq, const uint64_t *word, uint64_t seen, uint64_t deadline) {
    uint64_t f = irq_save();
    bool ok = wq_sleep(wq, word, seen, NULL, deadline);
    irq_restore(f);
    return ok;
}

/* wq's lock held. */
static void wake_all_held(struct wait_queue *wq) {
    struct task *t = wq->head;
    wq->head = NULL;
    while (t) {
        struct task *n = t->wq_next;
        t->wq_next = NULL;
        __atomic_store_n(&t->wq, NULL, __ATOMIC_RELEASE);
        sched_make_ready(t);
        t = n;
    }
}

void wq_wake_all(struct wait_queue *wq) {
    uint64_t f = spin_lock(&wq->lock);
    wake_all_held(wq);
    spin_unlock(&wq->lock, f);
}

void sleep_until(uint64_t deadline) {
    uint64_t f = irq_save();
    wq_sleep(NULL, NULL, 0, NULL, deadline);
    irq_restore(f);
}

void mutex_lock(struct mutex *m) {
    struct task *self = current_task;
    uint64_t f = spin_lock(&m->wait.lock);
    while (m->owner && m->owner != self) {
        wq_sleep(&m->wait, NULL, 0, &m->wait.lock, CLOCK_NEVER);
        spin_take(&m->wait.lock);
    }
    m->owner = self;
    m->depth++;
    spin_unlock(&m->wait.lock, f);
}

void mutex_unlock(struct mutex *m) {
    uint64_t f = spin_lock(&m->wait.lock);
    if (m->owner != current_task || !m->depth) panic("mutex: released by a task that does not hold it");
    if (!--m->depth) {
        m->owner = NULL;
        wake_all_held(&m->wait);
    }
    spin_unlock(&m->wait.lock, f);
}

bool mutex_held(struct mutex *m) { return __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == current_task; }

void mutex_wait(struct mutex *m, struct wait_queue *wq) {
    if (m->owner != current_task || m->depth != 1) panic("mutex: waited on by a task that does not hold it once");
    uint64_t f = spin_lock(&wq->lock);
    mutex_unlock(m);
    wq_wait_locked(wq, &wq->lock);
    irq_restore(f);
    mutex_lock(m);
}

/* ---- threads ---- */

int task_thread_create(uint64_t entry, uint64_t function, uint64_t argument, uint64_t stack_bytes) {
    struct task *owner = task_process();
    if (!owner->is_user || owner->exiting || owner->killed) return -N_ERR_STOPPED;
    /* No size cap of its own: the address region and free memory bound it. */
    if (stack_bytes > UINT64_MAX - 3 * PAGE_SIZE) return -N_ERR_INVALID;
    stack_bytes = stack_bytes ? ALIGN_UP(stack_bytes, PAGE_SIZE) : THREAD_STACK_DEFAULT;
    unsigned count = 0;
    uint64_t f = spin_lock(&task_lock);
    for (struct task *p = task_list; p; p = p->all_next)
        if (p->is_thread && p->process == owner && p->state != TASK_ZOMBIE) count++;
    spin_unlock(&task_lock, f);
    if (count >= 128) return -N_ERR_NOT_READY;
    struct task *t = task_alloc(owner->name);
    if (!t) return -N_ERR_NO_MEMORY;
    t->is_user = t->is_thread = true;
    t->process = t->parent = owner;
    t->pml4 = owner->pml4;
    t->user_stack_size = stack_bytes;
    /* Unmapped page at each end, distinct from every other thread's stack. */
    uint64_t reserved = mmap_reserve(owner, t->user_stack_size + PAGE_SIZE);
    if (reserved) t->user_stack = reserved + PAGE_SIZE; /* task_free gives the reservation back */
    if (!reserved || vmm_user_reserve(t->pml4, t->user_stack, t->user_stack_size, VM_W) < 0) {
        f = spin_lock(&task_lock);
        task_unlink_locked(t);
        spin_unlock(&task_lock, f);
        task_free(t);
        return -N_ERR_NO_MEMORY;
    }
    extern void user_trampoline(void);
    struct regs frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = entry;
    frame.cs = USER_CS;
    frame.ss = USER_DS;
    frame.rflags = 0x202;
    frame.rsp = t->user_stack + t->user_stack_size - 8;
    frame.rdi = function;
    frame.rsi = argument;
    t->ksp -= sizeof frame;
    memcpy((void *)t->ksp, &frame, sizeof frame);
    push(t, (uint64_t)user_trampoline);
    for (unsigned i = 0; i < 6; i++) push(t, 0);
    int tid = t->pid;
    sched_make_ready(t);
    return tid;
}

static struct task *thread_of_locked(struct task *owner, int tid) {
    for (struct task *p = task_list; p; p = p->all_next)
        if (p->pid == tid && p->is_thread && p->process == owner) return p;
    return NULL;
}

int task_thread_detach(int tid) {
    struct task *owner = task_process();
    uint64_t f = spin_lock(&task_lock);
    struct task *t = thread_of_locked(owner, tid);
    int r = 0;
    if (!t || t->joining || t->detached) r = -N_ERR_INVALID;
    else if (t->state == TASK_ZOMBIE) reap_locked(t); /* it has ended already */
    else t->detached = true;                         /* its end gives it to the reaper */
    spin_unlock(&task_lock, f);
    return r;
}

int task_thread_join(int tid) {
    struct task *owner = task_process();
    uint64_t f = spin_lock(&task_lock);
    struct task *t = thread_of_locked(owner, tid);
    if (!t || t == current_task || t->joining || t->detached) {
        spin_unlock(&task_lock, f);
        return -N_ERR_INVALID;
    }
    t->joining = true;
    for (;;) {
        if (current_task->killed || owner->exiting) {
            t->joining = false;
            spin_unlock(&task_lock, f);
            wq_wake_all(&owner->child_wait);
            return -N_ERR_STOPPED;
        }
        if (t->state == TASK_ZOMBIE) break;
        wq_wait_locked(&owner->child_wait, &task_lock);
        spin_take(&task_lock);
    }
    task_unlink_locked(t);
    spin_unlock(&task_lock, f);
    task_free(t);
    return 0;
}

/* ---- address waits ----
   Address waits are keyed by the physical word, so threads of one process and processes mapping
   the same shared page meet in one waiter list. Blocked tasks and asynchronous watches share one
   enrollment sequence: a counted notify wakes the oldest waiters first, whichever kind they are.
   The kernel reads the word through the direct map at its physical address, never at the app's: a
   sibling thread may take the page away meanwhile. */
struct address_watch_queue;
struct address_watch {
    struct address_watch *next;
    struct address_watch_queue *queue;
    uint64_t phys, seq, token;
};
struct address_watch_queue {
    struct address_watch *ready, **ready_tail;
    struct address_watch_queue *notify_next; /* task_wake_address: notified once its lock is dropped */
    bool notify_pending;
    struct object object;
};
static struct wait_queue address_wait;
static struct address_watch *address_watches, **address_watch_tail = &address_watches;
static uint64_t address_seq;

/* How many waiters (tasks and watches) are enrolled per hash of the physical
   key. Enrollment raises the count before it reads the user value, and a waker
   reads the count after the user's store (with a full fence between), so
   either the waiter sees the new value or the waker sees the count. */
#define ADDRESS_BUCKETS 256
static uint32_t address_waiters[ADDRESS_BUCKETS];
static inline unsigned address_bucket_index(uint64_t phys) {
    return (unsigned)(((phys >> 2) * 0x9E3779B97F4A7C15ULL) >> 56);
}
static inline uint32_t *address_bucket(uint64_t phys) { return &address_waiters[address_bucket_index(phys)]; }

/* address_lock guards every waiter of a word: the tasks blocked on it (address_wait, whose own
   lock comes after it) and the watches. */
static struct spinlock address_lock;

/* The word's physical address in the calling app's space, its page made present; 0 when it is no
   memory of the app's, or not aligned. */
static uint64_t address_phys(volatile void *address, bool wide) {
    uint64_t va = (uint64_t)address;
    if ((va & (wide ? 7 : 3)) || va >= USER_TOP) return 0;
    uint64_t pml4 = current_task->pml4, phys = vmm_translate(pml4, va);
    if (!phys && vmm_demand_fault(pml4, va, false) > 0) phys = vmm_translate(pml4, va);
    return phys;
}
static uint64_t address_read(uint64_t phys, bool wide) {
    void *word = phys_to_virt(phys);
    return wide ? __atomic_load_n((volatile uint64_t *)word, __ATOMIC_SEQ_CST)
                : __atomic_load_n((volatile uint32_t *)word, __ATOMIC_SEQ_CST);
}

int task_wait_address(volatile void *address, uint64_t expected, bool wide, uint64_t deadline) {
    struct task *t = current_task;
    if (t->killed) return -N_ERR_STOPPED;
    uint64_t phys = address_phys(address, wide);
    if (!phys) return -N_ERR_BAD_ADDRESS;
    /* Counted, read and enrolled under address_lock, which a waker holds while it looks: either it
       finds this task or this task reads its new value. */
    uint32_t *bucket = address_bucket(phys);
    uint64_t flags = spin_lock(&address_lock);
    __atomic_add_fetch(bucket, 1, __ATOMIC_SEQ_CST);
    uint64_t current = address_read(phys, wide);
    if (current != expected || clock_now_ns() >= deadline) {
        __atomic_sub_fetch(bucket, 1, __ATOMIC_RELEASE);
        spin_unlock(&address_lock, flags);
        return current != expected ? -N_ERR_NOT_READY : -N_ERR_TIMEOUT;
    }
    t->wait_phys = phys;
    t->wait_seq = __atomic_add_fetch(&address_seq, 1, __ATOMIC_SEQ_CST);
    bool awake = wq_sleep(&address_wait, NULL, 0, &address_lock, deadline);
    t->wait_phys = 0;
    __atomic_sub_fetch(bucket, 1, __ATOMIC_RELEASE);
    irq_restore(flags);
    if (t->killed) return -N_ERR_STOPPED;
    return awake ? 0 : -N_ERR_TIMEOUT;
}

/* address_lock held. */
static void address_watch_unlink(struct address_watch **link) {
    struct address_watch *w = *link;
    *link = w->next;
    if (address_watch_tail == &w->next) address_watch_tail = link;
    w->next = NULL;
    __atomic_sub_fetch(address_bucket(w->phys), 1, __ATOMIC_RELEASE);
}

/* The oldest waiters of the physical word first, whether blocked or watching. A watch's queue is
   notified once address_lock is dropped, with a reference that keeps it meanwhile; a queue whose
   last reference is already gone is ending and is not notified. */
static unsigned address_wake(uint64_t phys, unsigned count) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* the waker's store before the count */
    if (!__atomic_load_n(&address_waiters[address_bucket_index(phys)], __ATOMIC_RELAXED)) return 0;
    unsigned done = 0;
    struct address_watch_queue *notify = NULL;
    uint64_t held = spin_lock(&address_lock);
    spin_take(&address_wait.lock);
    while (!count || done < count) {
        uint64_t best = UINT64_MAX;
        struct task **task_link = NULL;
        struct address_watch **watch_link = NULL;
        for (struct task **pp = &address_wait.head; *pp; pp = &(*pp)->wq_next)
            if ((*pp)->wait_phys == phys && (*pp)->wait_seq < best) { best = (*pp)->wait_seq; task_link = pp; }
        /* Watches are kept in enrollment order: the first match is the oldest. */
        for (struct address_watch **pw = &address_watches; *pw; pw = &(*pw)->next)
            if ((*pw)->phys == phys) {
                if ((*pw)->seq < best) { best = (*pw)->seq; watch_link = pw; task_link = NULL; }
                break;
            }
        if (watch_link) {
            struct address_watch *w = *watch_link;
            address_watch_unlink(watch_link);
            struct address_watch_queue *q = w->queue;
            __atomic_store_n(q->ready_tail, w, __ATOMIC_RELEASE); /* address_watch_pending */
            q->ready_tail = &w->next;
            if (!__atomic_load_n(&q->notify_pending, __ATOMIC_ACQUIRE) && object_ref_live(&q->object)) {
                q->notify_pending = true;
                q->notify_next = notify;
                notify = q;
            }
        } else if (task_link) {
            struct task *t = *task_link;
            *task_link = t->wq_next;
            t->wq_next = NULL;
            __atomic_store_n(&t->wq, NULL, __ATOMIC_RELEASE);
            sched_make_ready(t);
        } else break;
        done++;
    }
    spin_give(&address_wait.lock);
    spin_unlock(&address_lock, held);
    while (notify) {
        struct address_watch_queue *q = notify;
        notify = q->notify_next;
        /* next is read first: a waker that finds the flag clear chains q into its own list */
        __atomic_store_n(&q->notify_pending, false, __ATOMIC_RELEASE);
        object_notify(&q->object);
        object_unref(&q->object);
    }
    return done;
}

int task_wake_address(volatile uint32_t *address, unsigned count) {
    uint64_t va = (uint64_t)address;
    uint64_t phys = va < USER_TOP && !(va & 3) ? vmm_translate(current_task->pml4, va) : 0;
    return phys ? (int)address_wake(phys, count) : 0;
}

bool address_watch_pending(struct object *queue) {
    struct address_watch_queue *q = queue->priv;
    return __atomic_load_n(&q->ready, __ATOMIC_ACQUIRE) != NULL;
}

size_t address_watch_take(struct object *queue, uint64_t *tokens, size_t max) {
    struct address_watch_queue *q = queue->priv;
    struct address_watch *taken = NULL;
    size_t got = 0;
    uint64_t held = spin_lock(&address_lock);
    while (q->ready && got < max) {
        struct address_watch *w = q->ready;
        q->ready = w->next;
        if (!q->ready) q->ready_tail = &q->ready;
        tokens[got++] = w->token;
        w->next = taken;
        taken = w;
    }
    spin_unlock(&address_lock, held);
    while (taken) { struct address_watch *next = taken->next; kfree(taken); taken = next; }
    return got;
}
/* A waker's reference may be the last (address_wake). */
static void address_watch_release(struct object *o) {
    struct address_watch_queue *q = o->priv;
    struct address_watch *done = NULL;
    uint64_t flags = spin_lock(&address_lock);
    for (struct address_watch **pw = &address_watches; *pw;) {
        if ((*pw)->queue != q) { pw = &(*pw)->next; continue; }
        struct address_watch *w = *pw;
        address_watch_unlink(pw);
        w->next = done; done = w;
    }
    spin_unlock(&address_lock, flags);
    while (q->ready) { struct address_watch *next = q->ready->next; kfree(q->ready); q->ready = next; }
    while (done) { struct address_watch *next = done->next; kfree(done); done = next; }
    kfree(q);
}
static const struct object_ops address_watch_ops = {.kind = N_KIND_EVENTS, .release = address_watch_release};

struct object *address_watch_queue_new(void) {
    struct address_watch_queue *q = kzalloc(sizeof *q);
    if (!q) return NULL;
    q->ready_tail = &q->ready;
    object_init(&q->object, &address_watch_ops, q);
    return &q->object;
}

int address_watch_add(struct object *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    if (!queue || queue->ops != &address_watch_ops) return -N_ERR_NO_HANDLE;
    struct address_watch *w = kzalloc(sizeof *w);
    if (!w) return -N_ERR_NO_MEMORY;
    uint64_t phys = address_phys(address, false);
    if (!phys) { kfree(w); return -N_ERR_BAD_ADDRESS; }
    /* Counted before the value check and linked under address_lock, as task_wait_address; unlink
       uncounts. */
    uint64_t flags = spin_lock(&address_lock);
    __atomic_add_fetch(address_bucket(phys), 1, __ATOMIC_SEQ_CST);
    if (address_read(phys, false) != expected) {
        __atomic_sub_fetch(address_bucket(phys), 1, __ATOMIC_RELEASE);
        spin_unlock(&address_lock, flags);
        kfree(w);
        return -N_ERR_NOT_READY;
    }
    w->queue = queue->priv; w->phys = phys; w->seq = __atomic_add_fetch(&address_seq, 1, __ATOMIC_SEQ_CST); w->token = token;
    *address_watch_tail = w;
    address_watch_tail = &w->next;
    spin_unlock(&address_lock, flags);
    return 0;
}

int address_watch_remove(struct object *queue, uint64_t token) {
    if (!queue || queue->ops != &address_watch_ops) return -N_ERR_NO_HANDLE;
    struct address_watch_queue *q = queue->priv;
    struct address_watch *found = NULL;
    int result = 0;
    uint64_t flags = spin_lock(&address_lock);
    for (struct address_watch **pw = &address_watches; *pw; pw = &(*pw)->next)
        if ((*pw)->queue == q && (*pw)->token == token) { found = *pw; address_watch_unlink(pw); result = 1; break; }
    if (!found)
        for (struct address_watch **pw = &q->ready; *pw; pw = &(*pw)->next)
            if ((*pw)->token == token) {
                found = *pw; *pw = found->next;
                if (q->ready_tail == &found->next) q->ready_tail = pw;
                result = 2; break;
            }
    spin_unlock(&address_lock, flags);
    kfree(found);
    return result;
}

/* ---- ending ---- */

struct task *task_find(int pid) {
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid && t->state != TASK_ZOMBIE) return t;
    return NULL;
}

/* task_lock held. A task in ring 3 on another CPU hears of it at once; one blocked anywhere is
   woken (each wait looks at killed again). */
static void kill_locked(struct task *t) {
    if (!t->killed) t->kill_code = 130;
    __atomic_store_n(&t->killed, true, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST) == TASK_BLOCKED) {
        if (wq_take_blocked(t, false)) sched_make_ready(t);
    } else {
        cpu_kick(__atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE));
    }
}

NORETURN void task_exit_app(int code) {
    struct task *owner = task_process();
    if (current_task->is_thread && !owner->exiting) {
        /* Ending the app from one of its threads (ISO C exit, or a fault that may abandon a pool
           descriptor or a shared mutex) cannot be done by zombifying just this helper: the owner
           and every sibling are killed so the app ends with this code and no one waits for a dead
           holder. The owner's task_exit then joins them before destroying their shared mappings. */
        uint64_t f = spin_lock(&task_lock);
        for (struct task *t = task_list; t; t = t->all_next) {
            if (t == current_task || t->state == TASK_ZOMBIE || t->killed) continue;
            if (t == owner || (t->is_thread && t->process == owner)) {
                kill_locked(t);
                t->kill_code = code;
            }
        }
        spin_unlock(&task_lock, f);
    }
    task_exit(code);
}

static NORETURN void thread_exit(struct task *t) {
    /* The thread never returns to ring 3, so its user stack is dead now. An unjoined zombie keeps
       only its kernel stack and task record instead of its address space and the pages it touched. */
    struct task *owner = t->process;
    if (t->user_stack && owner && !owner->exiting && owner->pml4 == t->pml4 && t->pml4 != kernel_pml4) {
        uint64_t stack = t->user_stack, size = t->user_stack_size;
        t->user_stack = t->user_stack_size = 0;
        vmm_user_free(t->pml4, stack, size);
        mmap_hole_add(owner, stack - PAGE_SIZE, size + 2 * PAGE_SIZE);
    }
    /* Otherwise the app is ending and its space goes whole once its threads are joined. A detached
       thread's record may reach the reaper after the owner and its space are gone: it keeps no
       stack for task_free to give back. */
    t->user_stack = t->user_stack_size = 0;
    /* No shared descriptor closes, no child is orphaned, the VM stays: they are the app's. Its end
       is set under task_lock, which a joiner holds while it enrolls on child_wait. */
    cli();
    spin_take(&task_lock);
    t->state = TASK_ZOMBIE;
    if (t->detached) reap_locked(t);
    /* under task_lock: an owner that sees no thread left may end and be freed once it is dropped */
    wq_wake_all(&owner->child_wait);
    spin_give(&task_lock);
    schedule();
    panic("zombie thread %d was scheduled", t->pid);
}

NORETURN void task_exit(int code) {
    struct task *t = current_task;
    if (t->is_idle) panic("idle task tried to exit");
    t->exit_code = code;
    net_task_exit(t);
    sti();
    if (t->is_thread) thread_exit(t);
    t->exiting = true;
    /* Every thread of the app ends first, and is joined here: nothing reaches its VM or window
       buffers after that. */
    uint64_t f = spin_lock(&task_lock);
    for (;;) {
        struct task *alive = NULL, *ended = NULL;
        for (struct task *c = task_list; c; c = c->all_next) {
            if (!c->is_thread || c->process != t) continue;
            if (c->state != TASK_ZOMBIE) {
                if (!c->killed) kill_locked(c);
                alive = c;
            } else if (!c->joining) {
                ended = c;
                break;
            }
        }
        if (ended) {
            task_unlink_locked(ended);
            spin_unlock(&task_lock, f);
            task_free(ended);
            f = spin_lock(&task_lock);
            continue;
        }
        if (!alive) break;
        wq_wait_locked(&t->child_wait, &task_lock);
        spin_take(&task_lock);
    }
    spin_unlock(&task_lock, f);
    /* PCI claims first, whatever their handle numbers: a driver's device is stopped (and seen
       to have no DMA in flight) before the DMA objects among its other handles can free RAM. */
    for (int claims = 1; claims >= 0; claims--) {
        size_t at = 0;
        for (struct handle *h; (h = handle_table_next(t, &at, claims ? pci_user_is_claim : NULL));)
            handle_close(h);
    }
    wm_process_exit(t->pid);
    net_process_exit(t->pid);
    if (t->is_user && t->pml4 != kernel_pml4) {
        sys_shared_cleanup(t);
        uint64_t old = t->pml4;
        /* under task_lock: one who read the old space under it is done with its tables */
        f = spin_lock(&task_lock);
        t->pml4 = kernel_pml4;
        vmm_switch(kernel_pml4);
        spin_unlock(&task_lock, f);
        vmm_free_space(old);
    }
    struct object *app = t->app;
    t->app = NULL;
    if (app) {
        /* An app reports through its handle; no parent collects it. */
        proc_app_ended(app, t->exit_code);
        object_unref(app);
    }
    cli();
    spin_take(&task_lock);
    /* orphan our children */
    for (struct task *c = task_list, *next; c; c = next) {
        next = c->all_next;
        if (c->parent != t) continue;
        c->parent = NULL;
        if (c->state == TASK_ZOMBIE) reap_locked(c);
    }
    if (app && t->app_launched) {
        if (t->parent) wq_wake_all(&t->parent->child_wait);
        t->parent = NULL;
    }
    t->state = TASK_ZOMBIE;
    if (t->parent) wq_wake_all(&t->parent->child_wait);
    else reap_locked(t);
    spin_give(&task_lock);
    schedule();
    panic("zombie task %d was scheduled", t->pid);
}

int task_kill(int pid) {
    uint64_t f = spin_lock(&task_lock);
    struct task *t = task_find(pid);
    int r = 0;
    if (!t || t->is_idle) r = -N_ERR_NO_APP;
    else if (!t->is_user) r = -N_ERR_DENIED;
    else kill_locked(t);
    spin_unlock(&task_lock, f);
    return r;
}

int task_kill_tree(int pid, bool include_self) {
    int killed = 0;
    uint64_t f = spin_lock(&task_lock);
    for (struct task *t = task_list; t; t = t->all_next) {
        if (t->state == TASK_ZOMBIE || t->killed || !t->is_user) continue;
        bool hit = include_self && t->pid == pid;
        for (struct task *a = t->parent; a && !hit; a = a->parent) hit = a->pid == pid;
        if (hit) {
            kill_locked(t);
            killed++;
        }
    }
    spin_unlock(&task_lock, f);
    return killed;
}

int task_stop_apps(void) {
    int alive = 0;
    uint64_t f = spin_lock(&task_lock);
    for (struct task *t = task_list; t; t = t->all_next) {
        if (!t->is_user || t->state == TASK_ZOMBIE) continue;
        alive++;
        if (!t->killed) kill_locked(t);
    }
    spin_unlock(&task_lock, f);
    return alive;
}

int task_wait(int pid, int *status, bool nohang) {
    struct task *me = task_process();
    uint64_t f = spin_lock(&task_lock);
    for (;;) {
        if (current_task->killed || me->exiting) { spin_unlock(&task_lock, f); return -N_ERR_STOPPED; }
        bool have_child = false;
        for (struct task *c = task_list; c; c = c->all_next) {
            if (c->parent != me || c->is_thread || c->app_launched) continue;
            if (pid > 0 && c->pid != pid) continue;
            have_child = true;
            if (c->state == TASK_ZOMBIE) {
                int cpid = c->pid, code = c->exit_code;
                task_unlink_locked(c);
                spin_unlock(&task_lock, f);
                task_free(c);
                if (status) *status = code;
                return cpid;
            }
        }
        if (!have_child) { spin_unlock(&task_lock, f); return -N_ERR_NO_APP; }
        if (nohang) { spin_unlock(&task_lock, f); return 0; }
        wq_wait_locked(&me->child_wait, &task_lock);
        spin_take(&task_lock);
    }
}

void task_printf_stderr(struct task *t, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    struct task *owner = t->process ? t->process : t;
    struct handle *h = handle_get_of(owner, 2);
    if (!h) return;
    /* A message channel has no byte write: the text goes as one message. */
    if (message_channel_is(h)) message_channel_post(h, buf, (size_t)n, (uint64_t)owner->pid);
    else if (h->object->ops->write) h->object->ops->write(h, buf, (size_t)n, false);
    handle_close(h);
}
