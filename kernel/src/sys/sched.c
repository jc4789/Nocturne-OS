/* 4-level feedback queues; user-mode preemption, cooperative BSP kernel paths. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "gui/wm.h"
#include "net/net.h"
#include "abi.h"

static struct task *cpu_current[CPU_MAX_COUNT];
static volatile bool cpu_resched[CPU_MAX_COUNT];
struct task *task_list;
struct wait_queue poll_wq;

struct task **sched_current_slot(void) { return &cpu_current[cpu_current_index()]; }
volatile bool *sched_resched_slot(void) { return &cpu_resched[cpu_current_index()]; }

static struct task *idle_task;
#define MLFQ_LEVELS 4
static struct { struct task *head, *tail; } rq[CPU_MAX_COUNT][MLFQ_LEVELS];
static unsigned enqueue_cpu, dequeue_cpu;
static struct task *dead_list;
static int next_pid;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void kthread_trampoline(void);

static const uint32_t quantum_us[MLFQ_LEVELS] = {2000, 4000, 8000, 16000};
static const uint32_t allotment_us[MLFQ_LEVELS] = {8000, 16000, 32000, 64000};
#define BOOST_MS 250
static uint64_t boost_at, boost_epoch = 1;
static uint64_t bsp_stamp;
static bool bsp_stamp_tsc;

/* TSC subtraction also accounts IRQ-delayed/kernel CPU time and sub-ms bursts.
   Quotient/remainder avoids delta * 1000 overflow. Before calibration use ms. */
static uint64_t run_stamp(void) { return tsc_hz >= 1000000 ? rdtsc() : uptime_ms(); }
static uint64_t run_elapsed_us(uint64_t since, uint64_t now) {
    uint64_t delta = now - since;
    if (tsc_hz < 1000000) return delta > UINT64_MAX / 1000 ? UINT64_MAX : delta * 1000;
    uint64_t per_ms = tsc_hz / 1000;
    uint64_t whole = delta / per_ms;
    if (whole > UINT64_MAX / 1000) return UINT64_MAX;
    uint64_t fraction = (delta % per_ms) * 1000 / per_ms;
    uint64_t base = whole * 1000;
    return fraction > UINT64_MAX - base ? UINT64_MAX : base + fraction;
}

static void mlfq_reset(struct task *t) {
    t->sched_level = 0;
    t->sched_used_us = 0;
    t->slice_us = quantum_us[0];
    t->sched_epoch = boost_epoch;
}

/* Only BSP calls policy helpers, including after AP retire ACK. */
static bool mlfq_charge(struct task *t, uint64_t elapsed) {
    unsigned subms = (unsigned)(elapsed % 1000) + t->cpu_subms_us;
    t->cpu_ms += elapsed / 1000 + subms / 1000;
    t->cpu_subms_us = subms % 1000;
    if (t == idle_task) return false;
    if (t->sched_epoch != boost_epoch) mlfq_reset(t);
    bool expired = elapsed >= t->slice_us;
    t->slice_us = expired ? 0 : t->slice_us - (uint32_t)elapsed;
    /* A long non-preemptible kernel segment can cross several allotments.
       At the bottom use modulo, not a loop proportional to elapsed time. */
    while (elapsed) {
        uint32_t budget = allotment_us[t->sched_level];
        uint32_t left = budget - t->sched_used_us;
        if (elapsed < left) { t->sched_used_us += (uint32_t)elapsed; break; }
        elapsed -= left;
        t->sched_used_us = 0;
        if (t->sched_level + 1 == MLFQ_LEVELS) {
            t->sched_used_us = (uint32_t)(elapsed % budget);
            break;
        }
        t->sched_level++;
    }
    return expired;
}

static void bsp_account(void) {
    bool tsc = tsc_hz >= 1000000;
    uint64_t now = run_stamp();
    /* sched_init precedes timer calibration; never subtract different clocks. */
    if (tsc == bsp_stamp_tsc && mlfq_charge(current_task, run_elapsed_us(bsp_stamp, now)))
        need_resched = true;
    bsp_stamp = now;
    bsp_stamp_tsc = tsc;
}

static unsigned rq_highest(void) {
    for (unsigned i = 0; i < MLFQ_LEVELS; i++)
        for (unsigned cpu = 0; cpu < CPU_MAX_COUNT; cpu++) if (rq[cpu][i].head) return i;
    return MLFQ_LEVELS;
}

static void rq_append(struct task *t, unsigned cpu) {
    ASSERT(!t->on_rq && !t->owner_cpu && t != idle_task);
    if (t->sched_epoch != boost_epoch) mlfq_reset(t);
    unsigned level = t->sched_level;
    ASSERT(level < MLFQ_LEVELS);
    t->state = TASK_READY;
    t->on_rq = true;
    t->rq_cpu = cpu;
    t->rq_next = NULL;
    if (rq[cpu][level].tail) rq[cpu][level].tail->rq_next = t;
    else rq[cpu][level].head = t;
    rq[cpu][level].tail = t;
}
static void rq_push(struct task *t) {
    unsigned cpu = 0;
    if (t->is_user) {
        /* Only BSP mutates queues. APs receive sealed contexts, never a shared
           queue lock; round-robin allocation and dequeue permit work stealing. */
        for (unsigned n = 0; n < CPU_MAX_COUNT; n++) {
            unsigned candidate = (++enqueue_cpu) % CPU_MAX_COUNT;
            if (!candidate || cpu_runner_ready_at(candidate)) { cpu = candidate; break; }
        }
    }
    rq_append(t, cpu);
}

/* FIFO is retained within each priority. A periodic boost bounds starvation
   without a tick-time scan of every ready task or arbitrary name priorities. */
static void mlfq_boost(uint64_t now) {
    if (now - boost_at < BOOST_MS) return;
    boost_at = now;
    boost_epoch++;
    for (unsigned cpu = 0; cpu < CPU_MAX_COUNT; cpu++) {
    for (unsigned level = 0; level < MLFQ_LEVELS; level++) {
        struct task *t = rq[cpu][level].head;
        rq[cpu][level].head = rq[cpu][level].tail = NULL;
        while (t) {
            struct task *next = t->rq_next;
            t->on_rq = false;
            mlfq_reset(t);
            rq_append(t, cpu);
            t = next;
        }
    }
    }
    /* The loop above appends level 1..3 to already rebuilt level 0; it never
       reprocesses them. AP-owned policy is reset only after its ACK. */
    if (current_task != idle_task && !current_task->owner_cpu) mlfq_reset(current_task);
}

/* ready/wait/tasklist/allocatorはBSPのみ。APは一つのuser所有と固定mailboxだけ。
   phase release/acquireはksp/XSTATE保存とtaskstack離脱を含む所有移譲境界。 */
enum runner_phase { RUN_EMPTY, RUN_RESERVED, RUN_LAUNCH, RUN_USER, RUN_RETIRING, RUN_RETURNED };
static struct runner_mailbox {
    unsigned phase, stop;
    struct task *task;
    unsigned level;           /* immutable BSP snapshot until AP retire ACK */
    uint32_t quantum_us;
    uint64_t stamp, elapsed_us;
    unsigned ticks;           /* only fallback clock when no calibrated TSC */
} runner_mails[CPU_MAX_COUNT];
static uint64_t runner_idle_sp[CPU_MAX_COUNT];

static void request_priority_preemption(void) {
    unsigned ready = rq_highest();
    if (ready == MLFQ_LEVELS) return;
    if (current_task == idle_task || ready < current_task->sched_level) need_resched = true;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
    struct runner_mailbox *mail = &runner_mails[cpu];
    unsigned phase = __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE);
    if ((phase == RUN_LAUNCH || phase == RUN_USER) &&
        (ready < mail->level ||
         (mail->task->sched_epoch != boost_epoch && mail->level != 0)) &&
        !__atomic_load_n(&mail->stop, __ATOMIC_RELAXED)) {
        __atomic_store_n(&mail->stop, 1, __ATOMIC_RELEASE);
        cpu_runner_wake_at(cpu);
    }
    }
}

static void collect_runner(void) {
    cpu_runner_check();
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
    struct runner_mailbox *mail = &runner_mails[cpu];
    if (__atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) != RUN_RETURNED) continue;
    struct task *t = mail->task;
    ASSERT(t && t->owner_cpu != 0 && !t->on_rq);
    t->owner_cpu = 0;
    mlfq_charge(t, mail->elapsed_us);
    mail->task = NULL;
    __atomic_store_n(&mail->stop, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mail->phase, RUN_EMPTY, __ATOMIC_RELEASE);
    sched_make_ready(t);
    }
}

void sched_post_switch(void) {
    if (cpu_is_runner()) {
        struct runner_mailbox *mail = &runner_mails[cpu_current_index()];
        if (__atomic_load_n(&mail->phase, __ATOMIC_RELAXED) == RUN_RETIRING)
            __atomic_store_n(&mail->phase, RUN_RETURNED, __ATOMIC_RELEASE);
    } else if (cpu_current_index() == 0) {
        /* context_switchが旧taskのkspを書き、BSPが別stackへ移った後。 */
        for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
            struct runner_mailbox *mail = &runner_mails[cpu];
            if (__atomic_load_n(&mail->phase, __ATOMIC_RELAXED) != RUN_RESERVED) continue;
            __atomic_store_n(&mail->phase, RUN_LAUNCH, __ATOMIC_RELEASE);
            cpu_runner_wake_at(cpu);
        }
    }
}

NORETURN void sched_ap_loop(void) {
    cli();
    current_task = NULL;
    unsigned cpu = cpu_current_index();
    struct runner_mailbox *mail = &runner_mails[cpu];
    cpu_runner_started();
    for (;;) {
        cli();
        if (cpu_worker_poll()) continue;
        if (__atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) != RUN_LAUNCH) {
            __asm__ volatile("sti; hlt" : : : "memory");
            continue;
        }
        /* idleは固定kernel image/stackだけ。heapのtaskを読む前にglobalもflush。 */
        vmm_switch(kernel_pml4);
        vmm_flush_all();
        if (__atomic_load_n(&mail->stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&mail->phase, RUN_RETURNED, __ATOMIC_RELEASE);
            continue; /* user stackは一度も使用していない */
        }
        struct task *t = mail->task;
        current_task = t;
        tss_set_rsp0((uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE);
        vmm_switch(t->pml4);
        wrmsr(0xC0000100, t->tls_base);
        fpu_restore(&t->fpu);
        mail->ticks = 0;
        mail->stamp = run_stamp();
        __atomic_store_n(&mail->phase, RUN_USER, __ATOMIC_RELEASE);
        context_switch(&runner_idle_sp[cpu], t->ksp);
        /* AP trapがこの固定stackへ戻りpost_switchでACKした。tにはもう触れない。 */
    }
}

void sched_ap_interrupt(struct regs *r) {
    unsigned cpu = cpu_current_index();
    struct runner_mailbox *mail = &runner_mails[cpu];
    uint64_t vector = r->vector;
    if (vector >= 32 && vector != 0x80 && vector != 0xFF) lapic_eoi();
    if ((r->cs & 3) != 3) {
        if (vector < 32) cpu_worker_fault(r);
        return;
    }
    bool dispatch = vector < 32 || vector == 0x80;
    if (vector == VEC_TIMER) mail->ticks++;
    mail->elapsed_us = tsc_hz >= 1000000 ? run_elapsed_us(mail->stamp, run_stamp()) :
                            (uint64_t)mail->ticks * 1000;
    /* Read-only identity queries need no BSP kernel continuation. */
    if (vector == 0x80 && (r->rax == SYS_CPU_INDEX || r->rax == SYS_THREAD_ID ||
                          r->rax == SYS_GETPID || r->rax == SYS_UPTIME)) {
        r->rax = r->rax == SYS_CPU_INDEX ? cpu : r->rax == SYS_THREAD_ID ? current_task->pid :
                 r->rax == SYS_GETPID ? task_process()->pid : uptime_ms();
        dispatch = false;
    }
    if (!dispatch && !__atomic_load_n(&mail->stop, __ATOMIC_ACQUIRE) &&
        mail->elapsed_us < mail->quantum_us) return;
    struct task *t = current_task;
    t->trap_cpu = cpu;
    /* CR2はfaultを起こしたCPUでcapture。BSPのread_cr2は決して代用しない。 */
    uint64_t remote_cr2 = vector == 14 ? read_cr2() : 0;
    fpu_save(&t->fpu);
    vmm_switch(kernel_pml4);
    vmm_flush_all();
    current_task = NULL;
    __atomic_store_n(&mail->phase, RUN_RETIRING, __ATOMIC_RELAXED);
    context_switch(&t->ksp, runner_idle_sp[cpu]);
    /* このcontinuationはretire ACK→BSP ready queue→scheduleでBSPだけに戻す。 */
    cpu_require_bsp();
    if (dispatch) isr_dispatch_remote(r, remote_cr2);
    else sched_user_return(r);
}

void sched_quiesce_space(uint64_t pml4) {
    cpu_require_bsp();
    if (!cpu_runner_ready()) return;
    ASSERT(!cpu_jobs_active()); /* worker callbackでmappingの変更/解放は禁止 */
    uint64_t flags = irq_save();
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
    struct runner_mailbox *mail = &runner_mails[cpu];
    unsigned phase = __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE);
    if (phase != RUN_EMPTY && (!pml4 || mail->task->pml4 == pml4)) {
        ASSERT(phase != RUN_RESERVED); /* BSP stack-switch publication中はVM変更しない */
        __atomic_store_n(&mail->stop, 1, __ATOMIC_RELEASE);
        cpu_runner_wake_at(cpu);
        uint64_t since = rdtsc(), spins = 0;
        while (__atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) != RUN_RETURNED) {
            cpu_runner_check();
            if ((tsc_hz && rdtsc() - since > tsc_hz) || ++spins > 100000000)
                panic("smp: user retire timeout; task/space/context retained");
            pause();
        }
        collect_runner();
    }
    }
    /* IRQ待ちにBSPのqueue/allocator lockをAPへ要求しない。再入はRETURNEDを回収済み。 */
    irq_restore(flags);
}

void sched_pin_space(uint64_t pml4) {
    /* WM operations arrive as BSP-owned trap messages. Writable pixels are
       snapshotted by present, so having a window no longer pins user execution. */
    sched_quiesce_space(pml4);
}

void sched_init(void) {
    idle_task = kzalloc(sizeof(struct task));
    idle_task->pid = next_pid++;
    strlcpy(idle_task->name, "idle", sizeof idle_task->name);
    idle_task->state = TASK_RUNNING;
    idle_task->pml4 = kernel_pml4;
    strlcpy(idle_task->cwd, "/", sizeof idle_task->cwd);
    fpu_state_init(&idle_task->fpu);
    current_task = idle_task;
    task_list = idle_task;
    boost_at = uptime_ms();
    bsp_stamp = run_stamp();
    bsp_stamp_tsc = tsc_hz >= 1000000;
    kprintf("sched: MLFQ levels=4 quantum=2/4/8/16ms allotment=8/16/32/64ms boost=250ms\n");
}

struct task *task_alloc(const char *name) {
    struct task *t = kzalloc(sizeof(struct task));
    if (!t) return NULL;
    t->kstack = vmalloc(KSTACK_PAGES);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    uint64_t f = irq_save();
    t->pid = next_pid++;
    t->all_next = task_list;
    task_list = t;
    irq_restore(f);
    strlcpy(t->name, name, sizeof t->name);
    t->state = TASK_BLOCKED; /* not runnable until made ready */
    t->pml4 = kernel_pml4;
    strlcpy(t->cwd, "/", sizeof t->cwd);
    fpu_state_init(&t->fpu);
    t->start_ms = uptime_ms();
    mlfq_reset(t);
    t->ksp = (uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE;
    return t;
}

static void push(struct task *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) {
    struct task *t = task_alloc(name);
    if (!t) return NULL;
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

void kthread_start(void (*fn)(void *), void *arg) {
    sti();
    fn(arg);
    task_exit(0);
}

void sched_make_ready(struct task *t) {
    uint64_t f = irq_save();
    ASSERT(t->owner_cpu == 0);
    if (t->state != TASK_ZOMBIE && t != idle_task && !t->on_rq && t != current_task) {
        rq_push(t);
        request_priority_preemption();
    } else if (t == current_task && t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNING;
    }
    irq_restore(f);
}

static struct task *rq_pop(void) {
    unsigned level = rq_highest();
    if (level == MLFQ_LEVELS) return NULL;
    for (unsigned n = 0; n < CPU_MAX_COUNT; n++) {
        unsigned cpu = (++dequeue_cpu) % CPU_MAX_COUNT;
        struct task *t = rq[cpu][level].head;
        if (!t) continue;
        rq[cpu][level].head = t->rq_next;
        if (!rq[cpu][level].head) rq[cpu][level].tail = NULL;
        t->on_rq = false;
        t->rq_next = NULL;
        return t;
    }
    return NULL;
}

static void free_task(struct task *t) {
    ASSERT(!t->owner_cpu);
    /* unlink from the global list */
    struct task **pp = &task_list;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    if (t->is_thread && t->user_stack && t->pml4 != kernel_pml4)
        vmm_user_free(t->pml4, t->user_stack, t->user_stack_size);
    vfree(t->kstack, KSTACK_PAGES);
    kfree(t);
}

static void reap_dead(void) {
    while (dead_list) {
        struct task *t = dead_list;
        dead_list = t->rq_next;
        free_task(t);
    }
}

void schedule(void) {
    cpu_require_bsp();
    ASSERT(!cpu_jobs_active());
    uint64_t f = irq_save();
    bsp_account();
    collect_runner();
    mlfq_boost(uptime_ms());
    struct task *prev = current_task;
    if (prev->owner_cpu) {
        struct runner_mailbox *mail = &runner_mails[prev->owner_cpu];
        ASSERT(__atomic_load_n(&mail->phase, __ATOMIC_RELAXED) == RUN_RESERVED);
        /* schedule accounting can exhaust a final sub-ms BSP remainder. Seal
           the AP snapshot only now, before stack-switch launch publication. */
        if (prev->sched_epoch != boost_epoch) mlfq_reset(prev);
        if (!prev->slice_us) prev->slice_us = quantum_us[prev->sched_level];
        mail->level = prev->sched_level;
        mail->quantum_us = prev->slice_us;
    }
    if (prev->state == TASK_RUNNING && prev != idle_task && !prev->owner_cpu) {
        rq_push(prev);
    }
    struct task *next = rq_pop();
    if (!next) next = idle_task;
    need_resched = false;
    if (next != idle_task && !next->slice_us) next->slice_us = quantum_us[next->sched_level];
    if (next == prev) {
        prev->state = TASK_RUNNING;
        irq_restore(f);
        return;
    }
    next->state = TASK_RUNNING;
    current_task = next;
    bsp_stamp = run_stamp();
    tss_set_rsp0((uint64_t)next->kstack + KSTACK_PAGES * PAGE_SIZE);
    vmm_switch(next->pml4);
    wrmsr(0xC0000100, next->tls_base);
    fpu_save(&prev->fpu);
    fpu_restore(&next->fpu);
    context_switch(&prev->ksp, next->ksp);
    /* we are back in prev's context */
    if (!cpu_is_runner()) reap_dead();
    irq_restore(f);
}

void sched_yield(void) { schedule(); }

void sched_tick(void) {
    bsp_account();
    collect_runner();
    uint64_t now = uptime_ms();
    mlfq_boost(now);
    for (struct task *t = task_list; t; t = t->all_next) {
        if (t->state == TASK_BLOCKED && t->wake_at && now >= t->wake_at) {
            t->wake_at = 0;
            t->timed_out = true;
            /* remove from its wait queue, if any */
            if (t->wq) {
                struct task **pp = &t->wq->head;
                while (*pp && *pp != t) pp = &(*pp)->wq_next;
                if (*pp) *pp = t->wq_next;
                t->wq = NULL;
            }
            sched_make_ready(t);
        }
    }
    request_priority_preemption();
}

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatchはstiして返る。所有publicationからiretまでIRQを閉じる。 */
    cli();
    if (current_task->killed) task_exit(current_task->kill_code);
    if (need_resched) schedule();
    if (current_task->killed) task_exit(current_task->kill_code);
    struct task *t = current_task;
    if (!cpu_runner_ready() || !t->is_user) return;
    collect_runner();
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
    struct runner_mailbox *mail = &runner_mails[cpu];
    if (cpu_runner_ready_at(cpu) && __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) == RUN_EMPTY) {
        ASSERT(!(read_rflags() & 0x200) && !cpu_jobs_active());
        /* Finish BSP accounting before publishing the AP's remaining quantum. */
        bsp_account();
        if (!t->slice_us) t->slice_us = quantum_us[t->sched_level];
        t->owner_cpu = cpu;
        mail->task = t;
        mail->level = t->sched_level;
        mail->quantum_us = t->slice_us;
        mail->ticks = 0;
        mail->elapsed_us = 0;
        __atomic_store_n(&mail->phase, RUN_RESERVED, __ATOMIC_RELAXED);
        schedule();
        /* 最後の安全接点。APでresume後はshared scheduler/kernelを呼ばずiretへ。 */
        return;
    }
    }
}

/* ---- wait queues ---- */

void wq_wait(struct wait_queue *wq) {
    uint64_t f = irq_save();
    struct task *t = current_task;
    t->state = TASK_BLOCKED;
    t->wq = wq;
    t->wq_next = wq->head;
    wq->head = t;
    schedule();
    irq_restore(f);
}

bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms) {
    uint64_t f = irq_save();
    struct task *t = current_task;
    t->timed_out = false;
    uint64_t now = uptime_ms(), delay = ms ? ms : 1;
    t->wake_at = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
    if (wq) {
        t->wq = wq;
        t->wq_next = wq->head;
        wq->head = t;
    }
    t->state = TASK_BLOCKED;
    schedule();
    t->wake_at = 0;
    bool ok = !t->timed_out;
    irq_restore(f);
    return ok;
}

void wq_wake_all(struct wait_queue *wq) {
    uint64_t f = irq_save();
    struct task *t = wq->head;
    wq->head = NULL;
    while (t) {
        struct task *n = t->wq_next;
        t->wq_next = NULL;
        t->wq = NULL;
        t->wake_at = 0;
        sched_make_ready(t);
        t = n;
    }
    irq_restore(f);
}

void sleep_ms(uint64_t ms) { wq_wait_timeout(NULL, ms); }

/* ---- task lifecycle ---- */

static int task_thread_create_locked(uint64_t entry, uint64_t function, uint64_t argument) {
    struct task *owner = task_process();
    if (!owner->is_user || owner->exiting || owner->killed) return -EINTR;
    unsigned count = 0;
    for (struct task *p = task_list; p; p = p->all_next)
        if (p->is_thread && p->process == owner) count++;
    if (count >= 128) return -EAGAIN;
    struct task *t = task_alloc(owner->name);
    if (!t) return -ENOMEM;
    t->is_user = t->is_thread = true;
    t->process = t->parent = owner;
    t->pml4 = owner->pml4;
    t->user_stack_size = 256 * 1024;
    if (owner->mmap_next >= USER_STACK_TOP - USER_STACK_MAX - t->user_stack_size - 2 * PAGE_SIZE) {
        free_task(t);
        return -ENOMEM;
    }
    /* Unmapped page at each end, distinct from every other thread's stack. */
    t->user_stack = owner->mmap_next + PAGE_SIZE;
    owner->mmap_next = t->user_stack + t->user_stack_size + PAGE_SIZE;
    if (vmm_user_alloc(t->pml4, t->user_stack, t->user_stack_size, VM_W) < 0) {
        vmm_user_free(t->pml4, t->user_stack, t->user_stack_size);
        t->user_stack = 0;
        free_task(t);
        return -ENOMEM;
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
    sched_make_ready(t);
    return t->pid;
}

int task_thread_create(uint64_t entry, uint64_t function, uint64_t argument) {
    uint64_t flags = irq_save();
    int tid = task_thread_create_locked(entry, function, argument);
    irq_restore(flags);
    return tid;
}

int task_thread_join(int tid) {
    struct task *owner = task_process();
    struct task *t = NULL;
    uint64_t flags = irq_save();
    for (struct task *p = task_list; p; p = p->all_next)
        if (p->pid == tid && p->is_thread && p->process == owner) { t = p; break; }
    if (!t || t == current_task || t->joining) {
        irq_restore(flags);
        return -EINVAL;
    }
    t->joining = true;
    for (;;) {
        if (current_task->killed || owner->exiting) {
            t->joining = false;
            wq_wake_all(&owner->child_wait);
            irq_restore(flags);
            return -EINTR;
        }
        if (t->state == TASK_ZOMBIE) break;
        wq_wait(&owner->child_wait);
    }
    free_task(t);
    irq_restore(flags);
    return 0;
}

static struct wait_queue address_wait;
int task_wait_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms) {
    uint64_t flags = irq_save();
    /* Kill and enrollment are both BSP-owned. A queued/migrated syscall must
       never register an infinite wait after its kill wake already happened. */
    if (current_task->killed) { irq_restore(flags); return -EINTR; }
    if (__atomic_load_n(address, __ATOMIC_ACQUIRE) != expected) {
        irq_restore(flags);
        return -EAGAIN;
    }
    if (!timeout_ms) { irq_restore(flags); return -ETIMEDOUT; }
    struct task *t = current_task;
    t->wait_address = address;
    bool awake = true;
    if (timeout_ms == UINT32_MAX) wq_wait(&address_wait);
    else awake = wq_wait_timeout(&address_wait, timeout_ms);
    t->wait_address = NULL;
    irq_restore(flags);
    if (t->killed) return -EINTR;
    return awake ? 0 : -ETIMEDOUT;
}

int task_wake_address(volatile uint32_t *address, unsigned count) {
    uint64_t flags = irq_save();
    unsigned done = 0;
    struct task **pp = &address_wait.head;
    while (*pp) {
        struct task *t = *pp;
        if (t->pml4 == current_task->pml4 && t->wait_address == address) {
            *pp = t->wq_next;
            t->wq_next = NULL;
            t->wq = NULL;
            t->wake_at = 0;
            sched_make_ready(t);
            done++;
            if (count && done >= count) break;
        } else pp = &t->wq_next;
    }
    irq_restore(flags);
    return (int)done;
}

struct task *task_find(int pid) {
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid && t->state != TASK_ZOMBIE) return t;
    return NULL;
}

NORETURN void task_fault_exit(int code) {
    cpu_require_bsp();
    cli();
    struct task *owner = task_process();
    if (current_task->is_thread && !owner->exiting) {
        /* A fault may abandon a pool descriptor or a shared mutex. It cannot
           be recovered by zombifying just this helper: abort/wake the owner
           and every sibling so no user continuation waits for a dead holder.
           task_exit then joins them before destroying their shared mappings. */
        for (struct task *t = task_list; t; t = t->all_next) {
            if (t == current_task || t->state == TASK_ZOMBIE || t->killed) continue;
            if (t == owner || (t->is_thread && t->process == owner)) {
                if (task_kill(t->pid) == 0) t->kill_code = code;
            }
        }
    }
    task_exit(code);
}

NORETURN void task_exit(int code) {
    struct task *t = current_task;
    cpu_require_bsp();
    ASSERT(!t->owner_cpu);
    if (t == idle_task) panic("idle task tried to exit");
    t->exit_code = code;
    if (t->is_thread) {
        /* Do not close shared descriptors, orphan process children or destroy VM. */
        cli();
        t->state = TASK_ZOMBIE;
        wq_wake_all(&t->process->child_wait);
        schedule();
        panic("zombie thread %d was scheduled", t->pid);
    }
    /* SYS_EXIT enters with IRQs enabled. Keep the state test and child_wait
       enrollment atomic on the BSP: a sibling may otherwise exit during a
       timer preemption between those two operations and its last wake is lost.
       wq_wait switches to other tasks with their own interrupt state, so sibling
       syscall continuations can still finish while this owner is sleeping. */
    cli();
    t->exiting = true;
    /* Process exit first retires and joins every shared-space thread. Nothing
       can access its VM or window buffers after the last ACK and zombie join. */
    sched_quiesce_space(t->pml4);
    for (struct task *c = task_list; c; c = c->all_next)
        if (c->is_thread && c->process == t && c->state != TASK_ZOMBIE) task_kill(c->pid);
    for (;;) {
        struct task *child = NULL;
        for (struct task *c = task_list; c; c = c->all_next)
            if (c->is_thread && c->process == t) { child = c; break; }
        if (!child) break;
        if (child->state == TASK_ZOMBIE && !child->joining) free_task(child);
        else wq_wait(&t->child_wait);
    }
    sti();
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i]) {
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
        }
    }
    wm_process_exit(t->pid);
    net_process_exit(t->pid);
    cli();
    if (t->is_user && t->pml4 != kernel_pml4) {
        uint64_t old = t->pml4;
        t->pml4 = kernel_pml4;
        vmm_switch(kernel_pml4);
        vmm_free_space(old);
    }
    /* orphan our children */
    for (struct task *c = task_list; c; c = c->all_next) {
        if (c->parent == t) {
            c->parent = NULL;
            if (c->state == TASK_ZOMBIE) {
                c->rq_next = dead_list;
                dead_list = c;
            }
        }
    }
    t->state = TASK_ZOMBIE;
    if (t->parent) {
        wq_wake_all(&t->parent->child_wait);
    } else {
        t->rq_next = dead_list;
        dead_list = t;
    }
    poll_notify();
    schedule();
    panic("zombie task %d was scheduled", t->pid);
}

int task_kill(int pid) {
    uint64_t f = irq_save();
    struct task *t = task_find(pid);
    if (!t || t == idle_task) { irq_restore(f); return -ESRCH; }
    if (!t->is_user) { irq_restore(f); return -EPERM; }
    sched_quiesce_space(t->pml4);
    if (!t->killed) t->kill_code = 130;
    t->killed = true;
    if (t->state == TASK_BLOCKED) {
        if (t->wq) {
            struct task **pp = &t->wq->head;
            while (*pp && *pp != t) pp = &(*pp)->wq_next;
            if (*pp) *pp = t->wq_next;
            t->wq = NULL;
        }
        t->wake_at = 0;
        sched_make_ready(t);
    }
    irq_restore(f);
    return 0;
}

int task_wait(int pid, int *status, bool nohang) {
    struct task *me = task_process();
    for (;;) {
        uint64_t f = irq_save();
        if (current_task->killed || me->exiting) { irq_restore(f); return -EINTR; }
        bool have_child = false;
        for (struct task *c = task_list; c; c = c->all_next) {
            if (c->parent != me || c->is_thread) continue;
            if (pid > 0 && c->pid != pid) continue;
            have_child = true;
            if (c->state == TASK_ZOMBIE) {
                int cpid = c->pid;
                if (status) *status = c->exit_code;
                free_task(c);
                irq_restore(f);
                return cpid;
            }
        }
        if (!have_child) {
            irq_restore(f);
            return -ECHILD;
        }
        if (nohang) {
            irq_restore(f);
            return 0;
        }
        if (me->killed) {
            irq_restore(f);
            return -EINTR;
        }
        wq_wait(&me->child_wait);
        irq_restore(f);
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
    uint64_t flags = irq_save();
    struct file *file = owner->fds[2] ? vfs_dup(owner->fds[2]) : NULL;
    irq_restore(flags);
    if (file) { vfs_write(file, buf, n); vfs_close(file); }
}
