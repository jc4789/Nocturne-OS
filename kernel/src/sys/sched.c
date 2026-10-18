/* 朔Ⅱ：AP上のuser実行は持続会計と並列返済表で保護し（sys/saku.c）、残りの
   枠をバースト分布の指数で選ぶ。BSPのkernel継続は協調的。BSP上のuser実行は
   BSP自身の時計で交代する。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "arch/pstate.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "sys/proc.h"
#include "gui/wm.h"
#include "net/net.h"
#include "abi.h"

struct task *task_list;

static struct task *idle_task;
/* kq: kernel threadとBSP専用継続にいるuser task（起床・新規・APからの退避）。
   uq: ring3へ戻れるuser task。順位は朔Ⅱ（期限、指数、参照順）。 */
struct run_queue { struct task *head, *tail; };
static struct run_queue kq, uq;
static struct task *dead_list;
static int next_pid;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void kthread_trampoline(void);

#define BSP_USER_SLICE_US 2000  /* BSP上のring3同士の交代 */
#define UQ_STARVE_MS 4          /* kqが続いてもuqの仕事をこの間隔でBSPへ通す */
#define INDEX_RESIDENCY_US 1000 /* 指数による停止は一期間以上走った後だけ */
static uint64_t bsp_stamp, bsp_run_us, uq_served_ms, saku_ms;
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

/* Only BSP calls policy helpers, including after AP retire ACK. service:
   AP上のring3実行（朔Ⅱの返済）。BSP上の時間はバースト年齢だけに数える。 */
static void task_charge(struct task *t, uint64_t elapsed, bool service) {
    unsigned subms = (unsigned)(elapsed % 1000) + t->cpu_subms_us;
    t->cpu_ms += elapsed / 1000 + subms / 1000;
    t->cpu_subms_us = subms % 1000;
    if (t != idle_task && t->is_user) saku_charge(t, elapsed, service);
}

static void bsp_account(void) {
    bool tsc = tsc_hz >= 1000000;
    uint64_t now = run_stamp();
    /* sched_init precedes timer calibration; never subtract different clocks. */
    if (tsc == bsp_stamp_tsc) {
        uint64_t elapsed = run_elapsed_us(bsp_stamp, now);
        task_charge(current_task, elapsed, false);
        bsp_run_us += elapsed;
    }
    bsp_stamp = now;
    bsp_stamp_tsc = tsc;
}

/* ready/wait/tasklist/allocatorはBSPのみ。APは一つのuser所有と固定mailboxだけ。
   phase release/acquireはksp/XSTATE保存とtaskstack離脱を含む所有移譲境界。 */
enum runner_phase { RUN_EMPTY, RUN_RESERVED, RUN_LAUNCH, RUN_USER, RUN_RETIRING, RUN_RETURNED };
static struct runner_mailbox {
    unsigned phase, stop;
    bool kernel;              /* AP: BSP専用のkernel継続へ退いた（資格休止） */
    struct task *task;
    uint64_t stamp, elapsed_us;
    unsigned ticks;           /* only fallback clock when no calibrated TSC */
} runner_mails[CPU_MAX_COUNT];
static uint64_t runner_idle_sp[CPU_MAX_COUNT];
static unsigned runner_return_pending;

static unsigned free_runners(void) {
    unsigned n = 0;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++)
        if (cpu_runner_ready_at(cpu) &&
            __atomic_load_n(&runner_mails[cpu].phase, __ATOMIC_ACQUIRE) == RUN_EMPTY) n++;
    return n;
}

static void rq_push(struct task *t) {
    ASSERT(!t->on_rq && !t->owner_cpu && t != idle_task);
    struct run_queue *q = t->is_user && t->saku.ready ? &uq : &kq;
    if (q == &uq && !uq.head) uq_served_ms = uptime_ms();
    t->state = TASK_READY;
    t->on_rq = true;
    t->rq_next = NULL;
    if (q->tail) q->tail->rq_next = t;
    else q->head = t;
    q->tail = t;
}

static struct task *rq_take(struct run_queue *q, struct task *t, struct task *before) {
    if (before) before->rq_next = t->rq_next; else q->head = t->rq_next;
    if (q->tail == t) q->tail = before;
    t->on_rq = false;
    t->rq_next = NULL;
    return t;
}

/* BSPは台帳の外にある追加のCPUで、そこでの実行は返済に数えない。参照に対して
   実サービスが最も足りない主体（前払いが最小）へ渡し、台帳が保護するAP上の
   配分（Q/P）を補う。同点は朔Ⅱの順位。 */
static bool bsp_before(struct task *a, struct task *b, uint64_t now) {
    if (a->saku.advance != b->saku.advance) return a->saku.advance < b->saku.advance;
    return saku_order(a, SAKU_SWITCH_US, b, SAKU_SWITCH_US, now) > 0;
}

/* uqの最上位。to_ap: 空きAPへ渡す順（朔Ⅱの順位）、そうでなければBSPの順。同順位はFIFO。 */
static struct task *uq_best(struct task **before_out, uint64_t now, bool to_ap) {
    struct task *best = uq.head, *best_before = NULL;
    for (struct task *before = uq.head, *t = before ? before->rq_next : NULL; t; before = t, t = t->rq_next)
        if (to_ap ? saku_order(t, SAKU_SWITCH_US, best, SAKU_SWITCH_US, now) > 0 : bsp_before(t, best, now)) {
            best = t;
            best_before = before;
        }
    *before_out = best_before;
    return best;
}

static struct task *rq_pop(void) {
    uint64_t now = uptime_ms();
    if (!uq.head) uq_served_ms = now;
    /* uqの仕事はBSPで少しだけ走って空きAPへ移る。kernel継続が続いても、
       空きAPがあるか一定時間待たせたらuqを通す。 */
    bool to_ap = uq.head && free_runners();
    if (uq.head && (!kq.head || now - uq_served_ms >= UQ_STARVE_MS || to_ap)) {
        struct task *before;
        struct task *t = uq_best(&before, now, to_ap);
        uq_served_ms = now;
        return rq_take(&uq, t, before);
    }
    return kq.head ? rq_take(&kq, kq.head, NULL) : NULL;
}

/* BSPの切替要求。kernel threadは協調的なので切らない。 */
static void bsp_preempt_check(void) {
    struct task *cur = current_task;
    if (cur == idle_task) {
        if (kq.head || uq.head) need_resched = true;
        return;
    }
    if (!cur->is_user || cur->owner_cpu) return;
    if (kq.head || (uq.head && (bsp_run_us >= BSP_USER_SLICE_US || free_runners())))
        need_resched = true;
}

/* AP上の実行量をBSPの時計で期間ごとに確定する。最後の量はAP自身の計測。
   TSCの食い違いで先に多く払った分は戻さない（返済を取り消さない）。 */
static void ap_progress(struct runner_mailbox *mail, bool final) {
    struct task *t = mail->task;
    uint64_t e = __atomic_load_n(&mail->elapsed_us, __ATOMIC_RELAXED);
    if (!final && tsc_hz >= 1000000 &&
        __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) == RUN_USER) {
        uint64_t stamp = __atomic_load_n(&mail->stamp, __ATOMIC_RELAXED), now = rdtsc();
        if (now > stamp) {
            uint64_t estimate = run_elapsed_us(stamp, now);
            uint64_t bound = t->saku.progress_us + 2 * SAKU_PERIOD_US;
            if (estimate > bound) estimate = bound;
            if (estimate > e) e = estimate;
        }
    }
    if (e <= t->saku.progress_us) return;
    task_charge(t, e - t->saku.progress_us, true);
    t->saku.progress_us = e;
}

static void ap_progress_all(void) {
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        struct runner_mailbox *mail = &runner_mails[cpu];
        unsigned phase = __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE);
        if (phase == RUN_USER || phase == RUN_RETIRING) ap_progress(mail, false);
    }
}

static void stop_runner(unsigned cpu, bool forced) {
    __atomic_store_n(&runner_mails[cpu].stop, 1, __ATOMIC_RELEASE);
    cpu_runner_wake_at(cpu);
    saku_note_stop(forced);
}

/* 期間ごとの配置：返済期限の近い主体を必ずAPへ置き、空いた枠を指数の高い
   順に埋める。APで走っている期限前の主体は、上位の候補のために退かせる。 */
static void rebalance(uint64_t now) {
    static uint32_t sweep;
    sweep++;
    unsigned free = free_runners();
    for (;;) {
        /* 最上位の未処理候補：uqとBSP上のring3主体 */
        struct task *cand = NULL;
        for (struct task *t = uq.head; t; t = t->rq_next)
            if (t->saku.claim != sweep && (!cand || saku_order(t, SAKU_SWITCH_US, cand, SAKU_SWITCH_US, now) > 0))
                cand = t;
        struct task *cur = current_task;
        if (cur != idle_task && cur->is_user && cur->saku.ready && !cur->owner_cpu &&
            cur->state == TASK_RUNNING && cur->saku.claim != sweep &&
            (!cand || saku_order(cur, SAKU_SWITCH_US, cand, SAKU_SWITCH_US, now) > 0))
            cand = cur;
        if (!cand) return;
        cand->saku.claim = sweep;
        if (free) {
            free--;
            need_resched = true; /* BSPが候補を走らせ、sched_user_returnで空きAPへ渡す */
            continue;
        }
        /* 最下位の犠牲：期限前で、停止要求のないAP上の主体 */
        unsigned victim = 0;
        for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
            struct runner_mailbox *mail = &runner_mails[cpu];
            unsigned phase = __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE);
            if ((phase != RUN_USER && phase != RUN_LAUNCH) || __atomic_load_n(&mail->stop, __ATOMIC_RELAXED) ||
                saku_due(mail->task)) continue;
            if (!victim || saku_order(mail->task, 0, runner_mails[victim].task, 0, now) < 0) victim = cpu;
        }
        if (!victim) return;
        struct task *v = runner_mails[victim].task;
        if (saku_due(cand)) stop_runner(victim, true);
        else if (!saku_active() && v->saku.progress_us >= 8 * SAKU_PERIOD_US) stop_runner(victim, true);
        else if (v->saku.progress_us >= INDEX_RESIDENCY_US &&
                 saku_index_beats(cand, SAKU_SWITCH_US, v, 0, now)) stop_runner(victim, false);
        else return;
    }
}

static void runner_returned(struct runner_mailbox *mail) {
    /* The task stack/XSTATE must be saved and left before the BSP can own it.
       One outstanding doorbell covers all returned mailboxes. */
    __atomic_store_n(&mail->phase, RUN_RETURNED, __ATOMIC_RELEASE);
    if (!__atomic_exchange_n(&runner_return_pending, 1, __ATOMIC_ACQ_REL))
        cpu_bsp_wake();
}

static bool collect_runner(void) {
    bool collected = false;
    cpu_runner_check();
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
    struct runner_mailbox *mail = &runner_mails[cpu];
    if (__atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) != RUN_RETURNED) continue;
    struct task *t = mail->task;
    ASSERT(t && t->owner_cpu != 0 && !t->on_rq);
    t->owner_cpu = 0;
    ap_progress(mail, true);
    /* syscall/faultのBSP継続は資格休止。停止要求での退避はring3のまま。 */
    if (mail->kernel) saku_suspend(t);
    mail->task = NULL;
    __atomic_store_n(&mail->stop, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mail->phase, RUN_EMPTY, __ATOMIC_RELEASE);
    sched_make_ready(t);
    collected = true;
    }
    return collected;
}

static void runner_return_irq(struct regs *r) {
    (void)r;
    cpu_require_bsp();
    /* Clear before scanning: a retire racing with or following the scan must
       send a fresh IPI rather than disappear behind the consumed doorbell. */
    /* An exchange also orders the subsequent loads after clearing the flag.
       A release store alone permits store/load reordering: an AP could still
       see pending=1 after our scan missed its newly published mailbox. */
    (void)__atomic_exchange_n(&runner_return_pending, 0, __ATOMIC_ACQ_REL);
    net_ap_notify_bsp();
    if (collect_runner()) need_resched = true;
    /* Do not schedule inside a kernel IRQ: existing kernel continuations are
       non-preemptive. User return and idle's HLT return are safe switch points. */
}

void sched_post_switch(void) {
    if (cpu_is_runner()) {
        struct runner_mailbox *mail = &runner_mails[cpu_current_index()];
        if (__atomic_load_n(&mail->phase, __ATOMIC_RELAXED) == RUN_RETIRING)
            runner_returned(mail);
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
    sched_set_current(NULL);
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
        /* kernel mappingの変更はtlb_shootdownがこのCPUにも届けるので全消去は不要。
           user spaceの古いtagはvmm_switchがgen比較で破棄する。 */
        vmm_switch(kernel_pml4);
        if (__atomic_load_n(&mail->stop, __ATOMIC_ACQUIRE)) {
            runner_returned(mail);
            continue; /* user stackは一度も使用していない */
        }
        struct task *t = mail->task;
        sched_set_current(t);
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
    if (vector == VEC_TIMER) {
        prof_tick(r, (r->cs & 3) != 3);
        if (pstate_sampling()) pstate_tick();
    }
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
                          r->rax == SYS_APP_SELF || r->rax == SYS_UPTIME)) {
        r->rax = r->rax == SYS_CPU_INDEX ? cpu : r->rax == SYS_THREAD_ID ? current_task->pid :
                 r->rax == SYS_APP_SELF ? task_process()->pid : uptime_ms();
        dispatch = false;
    }
    else if (vector==0x80 && net_ap_syscall(r)) dispatch=false;
    else if (vector==0x80 && address_ap_syscall(r)) dispatch=false;
    /* APは自分で量子を判断しない。BSPが朔Ⅱの期間ごとに会計し、退かせる時だけ
       停止要求を送る（rebalance）。何も待たなければ同じ主体がキャッシュを
       温めたまま走り続ける。 */
    if (!dispatch && !__atomic_load_n(&mail->stop, __ATOMIC_ACQUIRE)) return;
    mail->kernel = dispatch;
    struct task *t = current_task;
    prof_ap_retire(vector, r->rax);
    t->trap_cpu = cpu;
    /* CR2はfaultを起こしたCPUでcapture。BSPのread_cr2は決して代用しない。 */
    uint64_t remote_cr2 = vector == 14 ? read_cr2() : 0;
    fpu_save(&t->fpu);
    vmm_switch(kernel_pml4); /* PCIDならtagは残り、同じspaceへの再開でTLBが温かい */
    sched_set_current(NULL);
    __atomic_store_n(&mail->phase, RUN_RETIRING, __ATOMIC_RELAXED);
    context_switch(&t->ksp, runner_idle_sp[cpu]);
    /* このcontinuationはretire ACK→BSP ready queue→scheduleでBSPだけに戻す。 */
    cpu_require_bsp();
    if (dispatch) isr_dispatch_remote(r, remote_cr2);
    else sched_user_return(r);
}

bool sched_runner_idle(unsigned cpu) {
    return cpu && cpu < CPU_MAX_COUNT &&
           __atomic_load_n(&runner_mails[cpu].phase, __ATOMIC_ACQUIRE) == RUN_EMPTY;
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

void sched_init(void) {
    vector_register(VEC_USER_RETURN, runner_return_irq);
    idle_task = kzalloc(sizeof(struct task));
    idle_task->fds=idle_task->initial_fds;idle_task->fd_capacity=MAX_FDS;
    idle_task->pid = next_pid++;
    strlcpy(idle_task->name, "idle", sizeof idle_task->name);
    idle_task->state = TASK_RUNNING;
    idle_task->pml4 = kernel_pml4;
    fpu_state_init(&idle_task->fpu);
    sched_set_current(idle_task);
    task_list = idle_task;
    bsp_stamp = run_stamp();
    bsp_stamp_tsc = tsc_hz >= 1000000;
    kprintf("sched: saku II (ledger starts with the AP runners); BSP user slice %uus\n", BSP_USER_SLICE_US);
}

struct task *task_alloc(const char *name) {
    struct task *t = kzalloc(sizeof(struct task));
    if (!t) return NULL;
    t->fds=t->initial_fds;t->fd_capacity=MAX_FDS;
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
        rq_push(t);
        bsp_preempt_check();
    } else if (t == current_task && t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNING;
    }
    irq_restore(f);
}

static void free_task(struct task *t) {
    ASSERT(!t->owner_cpu);
    net_task_exit(t);
    uint64_t flags = irq_save();
    saku_forget(t);
    irq_restore(flags);
    /* unlink from the global list */
    struct task **pp = &task_list;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    if (t->is_thread && t->user_stack && t->pml4 != kernel_pml4) {
        vmm_user_free(t->pml4, t->user_stack, t->user_stack_size);
        /* Both guard pages belong to the stack's reservation. */
        if (t->process && t->process != t && t->process->pml4 == t->pml4)
            mmap_hole_add(t->process, t->user_stack - PAGE_SIZE, t->user_stack_size + 2 * PAGE_SIZE);
    }
    vfree(t->kstack, KSTACK_PAGES);
    if(t->fds!=t->initial_fds)kfree(t->fds);
    kfree(t);
}

int task_fd_reserve(struct task *owner,size_t minimum) {
    const size_t limit=(size_t)INT32_MAX+1;
    if(minimum>limit || minimum>SIZE_MAX/sizeof *owner->fds)return -N_ERR_NO_MEMORY;
    if(minimum<=owner->fd_capacity)return 0;
    size_t capacity=owner->fd_capacity;
    while(capacity<minimum){if(capacity>limit/2){capacity=minimum;break;}capacity*=2;}
    struct file **fds=kzalloc(capacity*sizeof *fds);if(!fds)return -N_ERR_NO_MEMORY;
    memcpy(fds,owner->fds,owner->fd_capacity*sizeof *fds);
    if(owner->fds!=owner->initial_fds)kfree(owner->fds);
    owner->fds=fds;owner->fd_capacity=capacity;return 0;
}
int task_fd_find(struct task *owner) {
    for(size_t i=owner->fd_hint;i<owner->fd_capacity;i++)if(!owner->fds[i])return (int)i;
    size_t index=owner->fd_capacity;
    int e=task_fd_reserve(owner,index+1);return e?e:(int)index;
}
int task_fd_alloc(struct task *owner,struct file *file) {
    int fd=task_fd_find(owner);if(fd<0)return fd;
    owner->fds[fd]=file;owner->fd_hint=(size_t)fd+1;return fd;
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
    struct task *prev = current_task;
    if (prev->owner_cpu) {
        ASSERT(__atomic_load_n(&runner_mails[prev->owner_cpu].phase, __ATOMIC_RELAXED) == RUN_RESERVED);
    } else if (prev->is_user) {
        if (prev->state == TASK_BLOCKED) saku_block(prev);      /* バースト終端 */
        else if (prev->state == TASK_ZOMBIE) saku_exit(prev);   /* 打切り */
    }
    if (prev->state == TASK_RUNNING && prev != idle_task && !prev->owner_cpu) {
        rq_push(prev);
    }
    struct task *next = rq_pop();
    if (!next) next = idle_task;
    need_resched = false;
    bsp_run_us = 0;
    if (next == prev) {
        prev->state = TASK_RUNNING;
        irq_restore(f);
        return;
    }
    next->state = TASK_RUNNING;
    sched_set_current(next);
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
    if (saku_active() && now != saku_ms) {
        /* 朔Ⅱの期間境界：AP上の実行を払い、期限を閉じ、次の参照を発行して配置する。 */
        uint64_t started = rdtsc();
        saku_ms = now;
        ap_progress_all();
        saku_period(now);
        rebalance(now);
        if (tsc_hz >= 1000000) saku_note_tick(run_elapsed_us(started, rdtsc()));
    }
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
    bsp_preempt_check();
}

/* BSP上のring3がsyscall/faultでkernel継続へ入る：APからの退避と同じ資格休止。
   未払い票はBSP継続の間に払えないので取り消し、バースト年齢は続ける。 */
void sched_user_kernel_entry(void) {
    uint64_t f = irq_save();
    struct task *t = current_task;
    if (t->is_user && !t->owner_cpu && t->saku.ready) saku_suspend(t);
    irq_restore(f);
}

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatchはstiして返る。所有publicationからiretまでIRQを閉じる。 */
    cli();
    for (;;) {
    if (current_task->killed) task_exit(current_task->kill_code);
    if (need_resched) schedule();
    if (current_task->killed) task_exit(current_task->kill_code);
    struct task *t = current_task;
    if (!cpu_runner_ready() || !t->is_user) return;
    if (!saku_active()) {
        /* smp初期化はuser起動より前に全runnerを揃える。ここで台帳の幅が決まる。 */
        unsigned runners = 0;
        for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) runners += cpu_runner_ready_at(cpu);
        saku_start(runners, uptime_ms());
    }
    bool arrived = !t->saku.ready;
    saku_ready(t);
    collect_runner();
    /* Prefer the AP this task last ran on: its caches still hold its data. */
    unsigned home = t->trap_cpu > 0 && t->trap_cpu < CPU_MAX_COUNT ? t->trap_cpu : 1;
    for (unsigned n = 0; n < CPU_MAX_COUNT - 1; n++) {
    unsigned cpu = 1 + (home - 1 + n) % (CPU_MAX_COUNT - 1);
    struct runner_mailbox *mail = &runner_mails[cpu];
    if (cpu_runner_ready_at(cpu) && __atomic_load_n(&mail->phase, __ATOMIC_ACQUIRE) == RUN_EMPTY) {
        ASSERT(!(read_rflags() & 0x200) && !cpu_jobs_active());
        bsp_account(); /* BSP上の最後の量はバースト年齢として確定 */
        t->owner_cpu = cpu;
        t->saku.progress_us = 0;
        mail->task = t;
        mail->kernel = false;
        mail->ticks = 0;
        mail->elapsed_us = 0;
        __atomic_store_n(&mail->phase, RUN_RESERVED, __ATOMIC_RELAXED);
        schedule();
        /* 最後の安全接点。APでresume後はshared scheduler/kernelを呼ばずiretへ。 */
        return;
    }
    }
    /* 空きAPなし：起床直後か返済期限が近ければ、次の期間を待たずに配置を見直す。 */
    uint64_t now = uptime_ms();
    if (arrived || saku_due(t)) rebalance(now);
    /* BSPでring3を続ける。BSPの順でuqにより不足した主体があれば先に譲る
       （起床したkernel継続がBSPを経由するたびに、そのままBSPを取らない）。
       残る間はBSP_USER_SLICE_USごとに交代する。 */
    struct task *before;
    if (!uq.head || !bsp_before(uq_best(&before, now, false), t, now)) return;
    need_resched = true;
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
    if (!owner->is_user || owner->exiting || owner->killed) return -N_ERR_STOPPED;
    unsigned count = 0;
    for (struct task *p = task_list; p; p = p->all_next)
        if (p->is_thread && p->process == owner) count++;
    if (count >= 128) return -N_ERR_NOT_READY;
    struct task *t = task_alloc(owner->name);
    if (!t) return -N_ERR_NO_MEMORY;
    t->is_user = t->is_thread = true;
    t->process = t->parent = owner;
    t->pml4 = owner->pml4;
    t->user_stack_size = 256 * 1024;
    /* Unmapped page at each end, distinct from every other thread's stack. */
    uint64_t reserved = mmap_reserve(owner, t->user_stack_size + PAGE_SIZE);
    if (!reserved) {
        free_task(t);
        return -N_ERR_NO_MEMORY;
    }
    t->user_stack = reserved + PAGE_SIZE;
    if (vmm_user_alloc(t->pml4, t->user_stack, t->user_stack_size, VM_W) < 0) {
        free_task(t); /* frees the partial stack and returns its reservation */
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
        return -N_ERR_INVALID;
    }
    t->joining = true;
    for (;;) {
        if (current_task->killed || owner->exiting) {
            t->joining = false;
            wq_wake_all(&owner->child_wait);
            irq_restore(flags);
            return -N_ERR_STOPPED;
        }
        if (t->state == TASK_ZOMBIE) break;
        wq_wait(&owner->child_wait);
    }
    free_task(t);
    irq_restore(flags);
    return 0;
}

/* Address waits are keyed by the physical word, so threads of one process and
   processes mapping the same shared page meet in one waiter list. Blocked
   tasks and asynchronous watches share one enrollment sequence: a counted
   notify wakes the oldest waiters first, whichever kind they are. */
struct address_watch_queue;
struct address_watch {
    struct address_watch *next;
    struct address_watch_queue *queue;
    uint64_t phys, seq, token;
};
struct address_watch_queue { struct address_watch *ready, **ready_tail; struct vnode *vn; };
static struct wait_queue address_wait;
static struct address_watch *address_watches, **address_watch_tail = &address_watches;
static uint64_t address_seq;

/* How many waiters (tasks and watches) are enrolled per hash of the physical
   key. The lists stay BSP-owned; this count lets an AP finish a wake with no
   waiter at all without migrating to the BSP. Enrollment raises the count
   before it reads the user value, and an AP waker reads the count after the
   user's store (with a full fence between), so either the waiter sees the new
   value or the waker sees the count and takes the BSP path. */
#define ADDRESS_BUCKETS 256
static uint32_t address_waiters[ADDRESS_BUCKETS];
static inline uint32_t *address_bucket(uint64_t phys) {
    return &address_waiters[((phys >> 2) * 0x9E3779B97F4A7C15ULL) >> 56];
}

int task_wait_address(volatile void *address, uint64_t expected, bool wide, unsigned timeout_ms) {
    uint64_t flags = irq_save();
    /* Kill and enrollment are both BSP-owned. A queued/migrated syscall must
       never register an infinite wait after its kill wake already happened. */
    if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
    struct task *t = current_task;
    uint64_t phys = vmm_translate(t->pml4, (uint64_t)address);
    if (!phys) { irq_restore(flags); return -N_ERR_BAD_ADDRESS; }
    uint32_t *bucket = address_bucket(phys);
    __atomic_add_fetch(bucket, 1, __ATOMIC_SEQ_CST);
    uint64_t current = wide ? __atomic_load_n((volatile uint64_t *)address, __ATOMIC_SEQ_CST)
                            : __atomic_load_n((volatile uint32_t *)address, __ATOMIC_SEQ_CST);
    if (current != expected || !timeout_ms) {
        __atomic_sub_fetch(bucket, 1, __ATOMIC_RELEASE);
        irq_restore(flags);
        return current != expected ? -N_ERR_NOT_READY : -N_ERR_TIMEOUT;
    }
    t->wait_phys = phys;
    t->wait_seq = ++address_seq;
    bool awake = true;
    if (timeout_ms == UINT32_MAX) wq_wait(&address_wait);
    else awake = wq_wait_timeout(&address_wait, timeout_ms);
    t->wait_phys = 0;
    __atomic_sub_fetch(bucket, 1, __ATOMIC_RELEASE);
    irq_restore(flags);
    if (t->killed) return -N_ERR_STOPPED;
    return awake ? 0 : -N_ERR_TIMEOUT;
}

static void address_watch_unlink(struct address_watch **link) {
    struct address_watch *w = *link;
    *link = w->next;
    if (address_watch_tail == &w->next) address_watch_tail = link;
    w->next = NULL;
    __atomic_sub_fetch(address_bucket(w->phys), 1, __ATOMIC_RELEASE);
}

/* AP: the address calls that never enqueue anything. A wake with no waiter
   enrolled for its key, and a wait whose value already differs (or that may
   not block), complete here; everything else returns false for the BSP. */
bool address_ap_syscall(struct regs *r) {
    uint64_t a = r->rdi;
    if (r->rax == SYS_WAKE_ADDRESS) {
        if ((a & 3) || a >= USER_TOP - sizeof(uint32_t)) return false;
        uint64_t pte = vmm_get_pte(current_task->pml4, a);
        if ((pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) return false;
        __atomic_thread_fence(__ATOMIC_SEQ_CST); /* the user's store before the count */
        if (__atomic_load_n(address_bucket((pte & PTE_ADDR) | (a & 0xFFF)), __ATOMIC_RELAXED)) return false;
        r->rax = 0;
        return true;
    }
    if (r->rax != SYS_WAIT_ADDRESS && r->rax != SYS_WAIT_ADDRESS64) return false;
    bool wide = r->rax == SYS_WAIT_ADDRESS64;
    uint64_t size = wide ? sizeof(uint64_t) : sizeof(uint32_t);
    if ((a & (size - 1)) || a >= USER_TOP - size || current_task->killed) return false;
    if (!vmm_ap_uaccess_begin()) return false;
    uint64_t pte = vmm_get_pte(current_task->pml4, a);
    if ((pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) { vmm_ap_uaccess_end(); return false; }
    uint64_t current = wide ? __atomic_load_n((volatile uint64_t *)a, __ATOMIC_ACQUIRE)
                            : __atomic_load_n((volatile uint32_t *)a, __ATOMIC_ACQUIRE);
    vmm_ap_uaccess_end();
    uint64_t expected = wide ? r->rsi : (uint32_t)r->rsi;
    if (current != expected) { r->rax = (uint64_t)(int64_t)-N_ERR_NOT_READY; return true; }
    if (!(unsigned)r->rdx) { r->rax = (uint64_t)(int64_t)-N_ERR_TIMEOUT; return true; }
    return false;
}

int task_wake_address(volatile uint32_t *address, unsigned count) {
    uint64_t flags = irq_save();
    unsigned done = 0;
    uint64_t phys = vmm_translate(current_task->pml4, (uint64_t)address);
    while (phys && (!count || done < count)) {
        uint64_t best = UINT64_MAX;
        struct task **task_link = NULL;
        struct address_watch **watch_link = NULL;
        for (struct task **pp = &address_wait.head; *pp; pp = &(*pp)->wq_next)
            if ((*pp)->wait_phys == phys && (*pp)->wait_seq < best) { best = (*pp)->wait_seq; task_link = pp; }
        /* Watches are kept in enrollment order: the first match is the oldest. */
        for (struct address_watch **pw = &address_watches; *pw; pw = &(*pw)->next)
            if ((*pw)->phys == phys) { if ((*pw)->seq < best) { watch_link = pw; task_link = NULL; } break; }
        if (watch_link) {
            struct address_watch *w = *watch_link;
            address_watch_unlink(watch_link);
            *w->queue->ready_tail = w;
            w->queue->ready_tail = &w->next;
            vfs_poll_notify(w->queue->vn);
        } else if (task_link) {
            struct task *t = *task_link;
            *task_link = t->wq_next;
            t->wq_next = NULL;
            t->wq = NULL;
            t->wake_at = 0;
            sched_make_ready(t);
        } else break;
        done++;
    }
    irq_restore(flags);
    return (int)done;
}

static int64_t address_watch_read_user(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    (void)f; (void)off;
    struct address_watch_queue *q = v->priv;
    size_t want = n / sizeof(uint64_t), got = 0;
    if (!want) return -N_ERR_INVALID;
    struct address_watch *done = NULL;
    uint64_t flags = irq_save();
    if (!q->ready) { irq_restore(flags); return -N_ERR_NOT_READY; }
    if (!user_ok_w(buf, want * sizeof(uint64_t))) { irq_restore(flags); return -N_ERR_BAD_ADDRESS; }
    while (q->ready && got < want) {
        struct address_watch *w = q->ready;
        q->ready = w->next;
        if (!q->ready) q->ready_tail = &q->ready;
        ((uint64_t *)buf)[got++] = w->token;
        w->next = done; done = w;
    }
    irq_restore(flags);
    while (done) { struct address_watch *next = done->next; kfree(done); done = next; }
    return (int64_t)(got * sizeof(uint64_t));
}
/* Take up to max notified tokens into a kernel array; BSP IRQ guard held. */
size_t address_watch_take(struct vnode *v, uint64_t *tokens, size_t max) {
    struct address_watch_queue *q = v->priv;
    size_t got = 0;
    while (q->ready && got < max) {
        struct address_watch *w = q->ready;
        q->ready = w->next;
        if (!q->ready) q->ready_tail = &q->ready;
        tokens[got++] = w->token;
        kfree(w);
    }
    return got;
}
static bool address_watch_can_read(struct vnode *v, struct file *f) {
    (void)f;
    return ((struct address_watch_queue *)v->priv)->ready != NULL;
}
static void address_watch_release(struct vnode *v) {
    struct address_watch_queue *q = v->priv;
    struct address_watch *done = NULL;
    uint64_t flags = irq_save();
    for (struct address_watch **pw = &address_watches; *pw;) {
        if ((*pw)->queue != q) { pw = &(*pw)->next; continue; }
        struct address_watch *w = *pw;
        address_watch_unlink(pw);
        w->next = done; done = w;
    }
    irq_restore(flags);
    while (q->ready) { struct address_watch *next = q->ready->next; kfree(q->ready); q->ready = next; }
    while (done) { struct address_watch *next = done->next; kfree(done); done = next; }
    kfree(q);
    kfree(v);
}
static struct vnode_ops address_watch_ops = {
    .read_user = address_watch_read_user, .can_read = address_watch_can_read,
    .release = address_watch_release,
};

struct vnode *address_watch_queue_new(void) {
    struct vnode *v = kzalloc(sizeof *v);
    struct address_watch_queue *q = kzalloc(sizeof *q);
    if (!v || !q) { kfree(v); kfree(q); return NULL; }
    q->ready_tail = &q->ready;
    q->vn = v;
    v->type = VT_ADDRESS_WATCH; v->unlinked = true; v->ops = &address_watch_ops; v->priv = q;
    return v;
}

int address_watch_add(struct vnode *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    if (!queue || queue->type != VT_ADDRESS_WATCH) return -N_ERR_NO_HANDLE;
    struct address_watch *w = kzalloc(sizeof *w);
    if (!w) return -N_ERR_NO_MEMORY;
    uint64_t flags = irq_save();
    uint64_t phys = vmm_translate(current_task->pml4, (uint64_t)address);
    if (!phys) { irq_restore(flags); kfree(w); return -N_ERR_BAD_ADDRESS; }
    /* Counted before the value check, as task_wait_address; unlink uncounts. */
    __atomic_add_fetch(address_bucket(phys), 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != expected) {
        __atomic_sub_fetch(address_bucket(phys), 1, __ATOMIC_RELEASE);
        irq_restore(flags); kfree(w); return -N_ERR_NOT_READY;
    }
    w->queue = queue->priv; w->phys = phys; w->seq = ++address_seq; w->token = token;
    *address_watch_tail = w;
    address_watch_tail = &w->next;
    irq_restore(flags);
    return 0;
}

int address_watch_remove(struct vnode *queue, uint64_t token) {
    if (!queue || queue->type != VT_ADDRESS_WATCH) return -N_ERR_NO_HANDLE;
    struct address_watch_queue *q = queue->priv;
    struct address_watch *found = NULL;
    int result = 0;
    uint64_t flags = irq_save();
    for (struct address_watch **pw = &address_watches; *pw; pw = &(*pw)->next)
        if ((*pw)->queue == q && (*pw)->token == token) { found = *pw; address_watch_unlink(pw); result = 1; break; }
    if (!found)
        for (struct address_watch **pw = &q->ready; *pw; pw = &(*pw)->next)
            if ((*pw)->token == token) {
                found = *pw; *pw = found->next;
                if (q->ready_tail == &found->next) q->ready_tail = pw;
                result = 2; break;
            }
    irq_restore(flags);
    kfree(found);
    return result;
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
    net_task_exit(t);
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
    for (size_t i = 0; i < t->fd_capacity; i++) {
        if (t->fds[i]) {
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
        }
    }
    wm_process_exit(t->pid);
    net_process_exit(t->pid);
    cli();
    if (t->is_user && t->pml4 != kernel_pml4) {
        sys_shared_cleanup(t);
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
    struct vnode *app = t->app;
    if (app) {
        /* An app reports through its handle; no parent collects it. */
        t->app = NULL;
        proc_app_ended(app, t->exit_code);
        vnode_unref(app);
        if (t->app_launched) {
            if (t->parent) wq_wake_all(&t->parent->child_wait);
            t->parent = NULL;
        }
    }
    if (t->parent) {
        wq_wake_all(&t->parent->child_wait);
    } else {
        t->rq_next = dead_list;
        dead_list = t;
    }
    schedule();
    panic("zombie task %d was scheduled", t->pid);
}

int task_kill(int pid) {
    uint64_t f = irq_save();
    struct task *t = task_find(pid);
    if (!t || t == idle_task) { irq_restore(f); return -N_ERR_NO_APP; }
    if (!t->is_user) { irq_restore(f); return -N_ERR_DENIED; }
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
        if (current_task->killed || me->exiting) { irq_restore(f); return -N_ERR_STOPPED; }
        bool have_child = false;
        for (struct task *c = task_list; c; c = c->all_next) {
            if (c->parent != me || c->is_thread || c->app_launched) continue;
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
            return -N_ERR_NO_APP;
        }
        if (nohang) {
            irq_restore(f);
            return 0;
        }
        if (me->killed) {
            irq_restore(f);
            return -N_ERR_STOPPED;
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
    struct file *file = owner->fd_capacity>2 && owner->fds[2] ? vfs_dup(owner->fds[2]) : NULL;
    irq_restore(flags);
    if (file) { vfs_write(file, buf, n); vfs_close(file); }
}
