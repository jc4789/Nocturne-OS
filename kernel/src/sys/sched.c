/* Round-robin preemptive (for user mode) scheduler. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "gui/wm.h"
#include "net/net.h"

static struct task *cpu_current[CPU_MAX_COUNT];
static volatile bool cpu_resched[CPU_MAX_COUNT];
struct task *task_list;
struct wait_queue poll_wq;

struct task **sched_current_slot(void) { return &cpu_current[cpu_current_index()]; }
volatile bool *sched_resched_slot(void) { return &cpu_resched[cpu_current_index()]; }

static struct task *idle_task;
static struct task *rq_head, *rq_tail;
static struct task *dead_list;
static int next_pid;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void kthread_trampoline(void);

#define SLICE_MS 10

/* ready/wait/tasklist/allocatorはBSPのみ。APは一つのuser所有と固定mailboxだけ。
   phase release/acquireはksp/XSTATE保存とtaskstack離脱を含む所有移譲境界。 */
enum runner_phase { RUN_EMPTY, RUN_RESERVED, RUN_LAUNCH, RUN_USER, RUN_RETIRING, RUN_RETURNED };
static struct {
    unsigned phase, stop;
    struct task *task;
    unsigned ticks;
} runner_mail;
static uint64_t runner_idle_sp;

static void collect_runner(void) {
    cpu_runner_check();
    if (__atomic_load_n(&runner_mail.phase, __ATOMIC_ACQUIRE) != RUN_RETURNED) return;
    struct task *t = runner_mail.task;
    ASSERT(t && t->owner_cpu != 0 && !t->on_rq);
    t->owner_cpu = 0;
    t->cpu_ms += runner_mail.ticks;
    runner_mail.task = NULL;
    __atomic_store_n(&runner_mail.stop, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&runner_mail.phase, RUN_EMPTY, __ATOMIC_RELEASE);
    sched_make_ready(t);
}

void sched_post_switch(void) {
    if (cpu_is_runner()) {
        if (__atomic_load_n(&runner_mail.phase, __ATOMIC_RELAXED) == RUN_RETIRING)
            __atomic_store_n(&runner_mail.phase, RUN_RETURNED, __ATOMIC_RELEASE);
    } else if (cpu_current_index() == 0 &&
               __atomic_load_n(&runner_mail.phase, __ATOMIC_RELAXED) == RUN_RESERVED) {
        /* context_switchが旧taskのkspを書き、BSPが別stackへ移った後。 */
        __atomic_store_n(&runner_mail.phase, RUN_LAUNCH, __ATOMIC_RELEASE);
        cpu_runner_wake();
    }
}

NORETURN void sched_ap_loop(void) {
    cli();
    current_task = NULL;
    cpu_runner_started();
    for (;;) {
        cli();
        if (__atomic_load_n(&runner_mail.phase, __ATOMIC_ACQUIRE) != RUN_LAUNCH) {
            __asm__ volatile("sti; hlt" : : : "memory");
            continue;
        }
        /* idleは固定kernel image/stackだけ。heapのtaskを読む前にglobalもflush。 */
        vmm_switch(kernel_pml4);
        vmm_flush_all();
        if (__atomic_load_n(&runner_mail.stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&runner_mail.phase, RUN_RETURNED, __ATOMIC_RELEASE);
            continue; /* user stackは一度も使用していない */
        }
        struct task *t = runner_mail.task;
        current_task = t;
        tss_set_rsp0((uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE);
        vmm_switch(t->pml4);
        fpu_restore(&t->fpu);
        runner_mail.ticks = 0;
        __atomic_store_n(&runner_mail.phase, RUN_USER, __ATOMIC_RELEASE);
        context_switch(&runner_idle_sp, t->ksp);
        /* AP trapがこの固定stackへ戻りpost_switchでACKした。tにはもう触れない。 */
    }
}

void sched_ap_interrupt(struct regs *r) {
    uint64_t vector = r->vector;
    if (vector >= 32 && vector != 0x80 && vector != 0xFF) lapic_eoi();
    if ((r->cs & 3) != 3) {
        if (vector < 32) cpu_worker_fault(r);
        return;
    }
    bool dispatch = vector < 32 || vector == 0x80;
    if (vector == VEC_TIMER) runner_mail.ticks++;
    if (!dispatch && !__atomic_load_n(&runner_mail.stop, __ATOMIC_ACQUIRE) &&
        runner_mail.ticks < SLICE_MS) return;
    struct task *t = current_task;
    /* CR2はfaultを起こしたCPUでcapture。BSPのread_cr2は決して代用しない。 */
    uint64_t remote_cr2 = vector == 14 ? read_cr2() : 0;
    fpu_save(&t->fpu);
    vmm_switch(kernel_pml4);
    vmm_flush_all();
    current_task = NULL;
    __atomic_store_n(&runner_mail.phase, RUN_RETIRING, __ATOMIC_RELAXED);
    context_switch(&t->ksp, runner_idle_sp);
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
    unsigned phase = __atomic_load_n(&runner_mail.phase, __ATOMIC_ACQUIRE);
    if (phase != RUN_EMPTY && (!pml4 || runner_mail.task->pml4 == pml4)) {
        ASSERT(phase != RUN_RESERVED); /* BSP stack-switch publication中はVM変更しない */
        __atomic_store_n(&runner_mail.stop, 1, __ATOMIC_RELEASE);
        cpu_runner_wake();
        uint64_t since = rdtsc(), spins = 0;
        while (__atomic_load_n(&runner_mail.phase, __ATOMIC_ACQUIRE) != RUN_RETURNED) {
            cpu_runner_check();
            if ((tsc_hz && rdtsc() - since > tsc_hz) || ++spins > 100000000)
                panic("smp: user retire timeout; task/space/context retained");
            pause();
        }
        collect_runner();
    }
    /* IRQ待ちにBSPのqueue/allocator lockをAPへ要求しない。再入はRETURNEDを回収済み。 */
    irq_restore(flags);
}

void sched_pin_space(uint64_t pml4) {
    sched_quiesce_space(pml4);
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pml4 == pml4) t->bsp_only = true;
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
        t->state = TASK_READY;
        t->on_rq = true;
        t->rq_next = NULL;
        if (rq_tail) rq_tail->rq_next = t;
        else rq_head = t;
        rq_tail = t;
    } else if (t == current_task && t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNING;
    }
    irq_restore(f);
}

static struct task *rq_pop(void) {
    struct task *t = rq_head;
    if (t) {
        rq_head = t->rq_next;
        if (!rq_head) rq_tail = NULL;
        t->on_rq = false;
    }
    return t;
}

static void free_task(struct task *t) {
    ASSERT(!t->owner_cpu);
    /* unlink from the global list */
    struct task **pp = &task_list;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
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
    collect_runner();
    struct task *prev = current_task;
    if (prev->state == TASK_RUNNING && prev != idle_task && !prev->owner_cpu) {
        prev->state = TASK_READY;
        prev->on_rq = true;
        prev->rq_next = NULL;
        if (rq_tail) rq_tail->rq_next = prev;
        else rq_head = prev;
        rq_tail = prev;
    }
    struct task *next = rq_pop();
    if (!next) next = idle_task;
    need_resched = false;
    if (next == prev) {
        prev->state = TASK_RUNNING;
        irq_restore(f);
        return;
    }
    next->state = TASK_RUNNING;
    next->slice = SLICE_MS;
    current_task = next;
    tss_set_rsp0((uint64_t)next->kstack + KSTACK_PAGES * PAGE_SIZE);
    vmm_switch(next->pml4);
    fpu_save(&prev->fpu);
    fpu_restore(&next->fpu);
    context_switch(&prev->ksp, next->ksp);
    /* we are back in prev's context */
    if (!cpu_is_runner()) reap_dead();
    irq_restore(f);
}

void sched_yield(void) { schedule(); }

void sched_tick(void) {
    collect_runner();
    struct task *cur = current_task;
    cur->cpu_ms++;
    uint64_t now = uptime_ms();
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
    if (cur != idle_task && --cur->slice <= 0) need_resched = true;
}

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatchはstiして返る。所有publicationからiretまでIRQを閉じる。 */
    cli();
    if (current_task->killed) task_exit(130);
    if (need_resched) schedule();
    if (current_task->killed) task_exit(130);
    struct task *t = current_task;
    if (!cpu_runner_ready() || t->bsp_only || !t->is_user) return;
    /* create/map前からwindow FDを持つtaskをpin。close後もshared mapping履歴を保持。 */
    for (unsigned i = 0; i < MAX_FDS; i++) if (t->fds[i] && t->fds[i]->vn->type == VT_WINDOW) {
        t->bsp_only = true;
        return;
    }
    collect_runner();
    if (__atomic_load_n(&runner_mail.phase, __ATOMIC_ACQUIRE) == RUN_EMPTY && rq_head) {
        ASSERT(!(read_rflags() & 0x200) && !cpu_jobs_active());
        t->owner_cpu = cpu_runner_index();
        runner_mail.task = t;
        runner_mail.ticks = 0;
        __atomic_store_n(&runner_mail.phase, RUN_RESERVED, __ATOMIC_RELAXED);
        schedule();
        /* 最後の安全接点。APでresume後はshared scheduler/kernelを呼ばずiretへ。 */
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
    t->wake_at = uptime_ms() + (ms ? ms : 1);
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
        if (t->is_user) need_resched = true;
        t = n;
    }
    irq_restore(f);
}

void sleep_ms(uint64_t ms) { wq_wait_timeout(NULL, ms); }

/* ---- task lifecycle ---- */

struct task *task_find(int pid) {
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid && t->state != TASK_ZOMBIE) return t;
    return NULL;
}

NORETURN void task_exit(int code) {
    struct task *t = current_task;
    cpu_require_bsp();
    ASSERT(!t->owner_cpu);
    if (t == idle_task) panic("idle task tried to exit");
    t->exit_code = code;
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
    struct task *t = task_find(pid);
    if (!t || t == idle_task) return -ESRCH;
    if (!t->is_user) return -EPERM;
    uint64_t f = irq_save();
    sched_quiesce_space(t->pml4);
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
    struct task *me = current_task;
    for (;;) {
        uint64_t f = irq_save();
        bool have_child = false;
        for (struct task *c = task_list; c; c = c->all_next) {
            if (c->parent != me) continue;
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
    if (t->fds[2]) vfs_write(t->fds[2], buf, n);
}
