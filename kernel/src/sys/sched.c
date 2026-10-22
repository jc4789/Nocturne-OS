/* 継：ring3へ戻れるuser threadはAPごとのキュー（sys/kei.c）で仮想実行量の順に
   走り、APは自分のキューの中でBSPを経由せずに切り替える。空いたAPは他のAPの
   待機threadを取りに行く。BSPのkernel継続は協調的で、BSPはkernelの仕事がない
   間だけAPのキューで待つthreadを借りて走らせる。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "dev/timer.h"
#include "arch/pstate.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "dev/pciuser.h"
#include "sys/proc.h"
#include "gui/wm.h"
#include "net/net.h"
#include "abi.h"

struct task *task_list;

static struct task *idle_task;
/* kq: kernel threadとBSPのkernel継続にいるuser task（起床・新規・APからの返却）。 */
struct run_queue { struct task *head, *tail; };
static struct run_queue kq;
static struct task *dead_list;
/* The public app/thread IDs are signed 32-bit. Never wrap or reuse an ID:
   queued channel credentials remain meaningful after their sender ends. */
static uint32_t next_pid;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void kthread_trampoline(void);

#define BSP_USER_SLICE_US 2000    /* BSPが借りたring3の最長連続 */
#define BALANCE_MS 4              /* 過負荷のAPから軽いAPへの移送の間隔 */
#define KEI_SWITCH_COST_NS 10000  /* AP内の切替（XSAVE、CR3、キャッシュの回復）の見積り */
#define KEI_OVERHEAD_PPM 10000    /* 切替損失の目標1%：継続幅はおよそ99倍 */
static uint64_t bsp_stamp, bsp_run_us, balance_ms, kei_retention_v;
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
/* 較正済みTSCだけ。継の会計はns。 */
static uint64_t run_elapsed_ns(uint64_t since, uint64_t now) {
    uint64_t delta = now - since, per_ms = tsc_hz / 1000;
    uint64_t whole = delta / per_ms;
    if (whole > UINT64_MAX / 2000000) return UINT64_MAX / 2;
    return whole * 1000000 + (delta % per_ms) * 1000000 / per_ms;
}

/* The owner of t calls this: the BSP, or the AP whose queue holds t. */
static void task_cpu_time(struct task *t, uint64_t elapsed) {
    unsigned subms = (unsigned)(elapsed % 1000) + t->cpu_subms_us;
    t->cpu_ms += elapsed / 1000 + subms / 1000;
    t->cpu_subms_us = subms % 1000;
}

static void bsp_account(void) {
    bool tsc = tsc_hz >= 1000000;
    uint64_t now = run_stamp();
    /* sched_init precedes timer calibration; never subtract different clocks. */
    if (tsc == bsp_stamp_tsc) {
        uint64_t elapsed = run_elapsed_us(bsp_stamp, now);
        struct task *t = current_task;
        task_cpu_time(t, elapsed);
        /* 借りたthreadのBSP上の時間も本人の実行量。戻すAPで先行分になる。 */
        if (t->bsp_borrow) kei_charge_detached(&t->kei, elapsed > UINT64_MAX / 2000 ? UINT64_MAX / 2 : elapsed * 1000);
        bsp_run_us += elapsed;
    }
    bsp_stamp = now;
    bsp_stamp_tsc = tsc;
}

/* APごとのuser threadキュー。そのAP、BSP、仕事を取りに来た空きAPが、IRQを閉じた
   まま短く排他する。排他中に他CPUを待たない（TLBシュートダウンのACKも待たない）。
   二つを取るときはCPU番号の昇順。キューにいる間（owner_cpu≠0）taskの文脈と会計は
   そのAPのもの。BSPのkernel継続が要るtaskはreturnedでBSPへ返す。 */
enum { RET_NONE, RET_LOCAL, RET_DISPATCH, RET_STOP };
static struct cpu_queue {
    uint32_t lock;
    struct kei_rq rq;
    struct task *returned, *returned_tail;
    uint64_t stamp;           /* rq.currentを最後に精算したTSC */
    unsigned reason;          /* AP自身：直前の退避理由 */
} __attribute__((aligned(64))) cpuq[CPU_MAX_COUNT];
static uint64_t runner_idle_sp[CPU_MAX_COUNT];
static unsigned runner_return_pending;
static size_t queue_capacity, kei_users; /* 各ヒープは追跡中の全user thread以上 */
static struct task *bsp_handoff;         /* BSPのstackを離れた後にAPのキューへ入れる */
static uint64_t quiesce_pml4;            /* 0なら全user空間 */
static bool quiescing;
static struct sched_stats stats;
#define KEI_OWNER(k) ((struct task *)((char *)(k) - __builtin_offsetof(struct task, kei)))
#define STAT(f) __atomic_fetch_add(&stats.f, 1, __ATOMIC_RELAXED)

static void q_lock(struct cpu_queue *q) {
    while (__atomic_exchange_n(&q->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&q->lock, __ATOMIC_RELAXED)) pause();
}
static void q_unlock(struct cpu_queue *q) { __atomic_store_n(&q->lock, 0, __ATOMIC_RELEASE); }
static void q_lock2(unsigned a, unsigned b) {
    q_lock(&cpuq[a < b ? a : b]);
    if (a != b) q_lock(&cpuq[a < b ? b : a]);
}
static void q_unlock2(unsigned a, unsigned b) {
    q_unlock(&cpuq[a]);
    if (a != b) q_unlock(&cpuq[b]);
}
/* 排他なしの目安。最終判断はキューの排他の中で行う。 */
static size_t q_waiting(unsigned cpu) { return __atomic_load_n(&cpuq[cpu].rq.count, __ATOMIC_RELAXED); }
static size_t q_resident(unsigned cpu) {
    return q_waiting(cpu) + (__atomic_load_n(&cpuq[cpu].rq.current, __ATOMIC_RELAXED) != NULL);
}
static bool q_usable(unsigned cpu) {
    return cpu_runner_ready_at(cpu) && __atomic_load_n(&cpuq[cpu].rq.online, __ATOMIC_ACQUIRE);
}
static bool quiesce_match(const struct task *t) {
    return __atomic_load_n(&quiescing, __ATOMIC_ACQUIRE) && (!quiesce_pml4 || t->pml4 == quiesce_pml4);
}

/* BSP: 全キューのヒープを追跡中のuser thread数以上にする（移動先が満杯にならない）。 */
static bool queues_reserve(size_t need) {
    if (need <= queue_capacity) return true;
    size_t cap = queue_capacity ? queue_capacity : 64;
    while (cap < need) cap *= 2;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        if (!cpu_runner_ready_at(cpu)) continue;
        struct kei_task **heap = kzalloc(cap * sizeof *heap);
        if (!heap) return false;
        struct cpu_queue *q = &cpuq[cpu];
        q_lock(q);
        struct kei_task **old = q->rq.heap;
        enum kei_error e = old ? kei_rq_resize(&q->rq, heap, cap) :
                                 kei_rq_init(&q->rq, cpu, heap, cap, kei_retention_v, 0);
        q_unlock(q);
        if (e != KEI_OK) panic("kei: queue %u setup failed %d", cpu, e);
        if (old) kfree(old);
    }
    queue_capacity = cap;
    return true;
}

static void rq_push(struct task *t) {
    ASSERT(!t->on_rq && !t->owner_cpu && t != idle_task);
    t->state = TASK_READY;
    t->on_rq = true;
    t->rq_next = NULL;
    if (kq.tail) kq.tail->rq_next = t;
    else kq.head = t;
    kq.tail = t;
}

static struct task *rq_pop(void) {
    struct task *t = kq.head;
    if (!t) return NULL;
    kq.head = t->rq_next;
    if (!kq.head) kq.tail = NULL;
    t->on_rq = false;
    t->rq_next = NULL;
    return t;
}

static bool ap_idle_exists(void) {
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++)
        if (q_usable(cpu) && !q_resident(cpu)) return true;
    return false;
}

/* 待機taskの最も多いAP（同数なら番号の小さい方）。exceptは除く。 */
static unsigned busiest_queue(unsigned except) {
    unsigned best = 0;
    size_t most = 0;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        if (cpu == except || !q_usable(cpu)) continue;
        size_t n = q_waiting(cpu);
        if (n > most) { most = n; best = cpu; }
    }
    return best;
}

/* BSPの切替要求。kernel threadは協調的なので切らない。借りたring3は、kernelの
   仕事が来たとき、BSP_USER_SLICE_USを超えたとき、空いたAPがあるときに返す。 */
static void bsp_preempt_check(void) {
    struct task *cur = current_task;
    if (cur == idle_task) {
        if (kq.head || busiest_queue(0)) need_resched = true;
        return;
    }
    if (!cur->is_user || cur->owner_cpu) return;
    if (kq.head || (cur->bsp_borrow && (bsp_run_us >= BSP_USER_SLICE_US || ap_idle_exists())))
        need_resched = true;
}

/* 待機中のtaskを一つsrcからdstへ移す（相対差を保つ）。番号順に両方を排他する。 */
static bool move_waiting(unsigned src, unsigned dst) {
    struct cpu_queue *s = &cpuq[src], *d = &cpuq[dst];
    bool moved = false;
    q_lock2(src, dst);
    if (d->rq.online)
        for (size_t i = 0; i < s->rq.count; i++) {
            struct kei_task *k = s->rq.heap[i];
            if (quiesce_match(KEI_OWNER(k))) continue;
            if (kei_migrate(&s->rq, &d->rq, k) == KEI_OK) {
                KEI_OWNER(k)->owner_cpu = dst;
                moved = true;
            }
            break;
        }
    q_unlock2(src, dst);
    return moved;
}

/* BSP: 最も混んだAPから、次に走る順のtaskを一つ借りる。 */
static struct task *bsp_borrow_task(void) {
    if (!queue_capacity) return NULL;
    unsigned src = busiest_queue(0);
    if (!src) return NULL;
    struct cpu_queue *q = &cpuq[src];
    struct task *t = NULL;
    q_lock(q);
    if (q->rq.count) {
        struct kei_task *k = q->rq.heap[0];
        if (kei_sleep(&q->rq, k) == KEI_OK) t = KEI_OWNER(k);
    }
    q_unlock(q);
    if (!t) return NULL;
    t->owner_cpu = 0;
    t->bsp_borrow = true;
    STAT(borrows);
    return t;
}

/* BSP: 過負荷のAPから最も軽いAPへ、待機taskを一つ移す（低頻度）。 */
static void ap_balance(void) {
    unsigned hi = 0, lo = 0;
    size_t hi_n = 0, lo_n = SIZE_MAX;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        if (!q_usable(cpu)) continue;
        size_t n = q_resident(cpu);
        if (q_waiting(cpu) && n > hi_n) { hi = cpu; hi_n = n; }
        if (n < lo_n) { lo = cpu; lo_n = n; }
    }
    if (!hi || !lo || hi == lo || hi_n < lo_n + 2) return;
    if (!move_waiting(hi, lo)) return;
    STAT(balances);
    cpu_runner_wake_at(lo);
}

/* AP: 現在のtaskの実行時間を精算する。キューの排他下。 */
static void ap_charge(struct cpu_queue *q, struct task *t, bool tick) {
    uint64_t ns = 0;
    if (tsc_hz >= 1000000) {
        uint64_t now = rdtsc();
        if (now > q->stamp) ns = run_elapsed_ns(q->stamp, now);
        q->stamp = now;
    } else if (tick) {
        ns = 1000000;
    }
    if (!ns || kei_charge(&q->rq, ns) != KEI_OK) return;
    task_cpu_time(t, ns / 1000);
}

static void ap_return_locked(struct cpu_queue *q, struct task *t) {
    t->rq_next = NULL;
    if (q->returned_tail) q->returned_tail->rq_next = t;
    else q->returned = t;
    q->returned_tail = t;
}

static void ap_doorbell(void) {
    /* One outstanding doorbell covers every queue's returned list. */
    if (!__atomic_exchange_n(&runner_return_pending, 1, __ATOMIC_ACQ_REL))
        cpu_bsp_wake();
}

/* AP: 次に走るtaskを確定する。停止中の空間のtaskは走らせずBSPへ返す。
   APではpanicしない（BSP所有のログへ触れない）。#UDでBSPが故障を報告する。 */
static struct task *ap_pick(struct cpu_queue *q, bool *returned) {
    for (;;) {
        struct kei_decision d = kei_decide(&q->rq);
        if (!d.next) return NULL;
        if (kei_commit(&q->rq, d.next) != KEI_OK) __builtin_trap();
        struct task *t = KEI_OWNER(d.next);
        if (!quiesce_match(t)) return t;
        kei_context_quiesced(&q->rq, d.next);
        kei_sleep(&q->rq, d.next);
        ap_return_locked(q, t);
        STAT(stop_retires);
        *returned = true;
    }
}

/* AP: 固定stackへ戻った後。tのstackはもう使っていない。 */
static void ap_retired(struct cpu_queue *q, struct task *t) {
    unsigned reason = q->reason;
    q_lock(q);
    if (kei_context_quiesced(&q->rq, &t->kei) != KEI_OK) __builtin_trap();
    if (reason != RET_LOCAL) {
        kei_sleep(&q->rq, &t->kei);
        ap_return_locked(q, t);
    }
    q_unlock(q);
    if (reason == RET_LOCAL) STAT(local_switches);
    else if (reason == RET_DISPATCH) STAT(dispatch_retires);
    else STAT(stop_retires);
    if (reason != RET_LOCAL) ap_doorbell();
}

static bool collect_runner(void) {
    bool collected = false;
    cpu_runner_check();
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        struct cpu_queue *q = &cpuq[cpu];
        if (!__atomic_load_n(&q->returned, __ATOMIC_RELAXED)) continue;
        q_lock(q);
        struct task *t = q->returned;
        q->returned = q->returned_tail = NULL;
        q_unlock(q);
        while (t) {
            struct task *next = t->rq_next;
            t->rq_next = NULL;
            t->owner_cpu = 0;
            sched_make_ready(t);
            collected = true;
            t = next;
        }
    }
    return collected;
}

static void runner_return_irq(struct regs *r) {
    (void)r;
    cpu_require_bsp();
    /* Clear before scanning: a return racing with or following the scan must
       send a fresh IPI rather than disappear behind the consumed doorbell. */
    (void)__atomic_exchange_n(&runner_return_pending, 0, __ATOMIC_ACQ_REL);
    net_ap_notify_bsp();
    if (collect_runner()) need_resched = true;
    /* Do not schedule inside a kernel IRQ: existing kernel continuations are
       non-preemptive. User return and idle's HLT return are safe switch points. */
}

/* BSP: 移す先。前回のAPを同点で優先し、それより軽いAPがあればそちら。 */
static unsigned place(struct task *t) {
    unsigned prev = t->kei.home ? (unsigned)t->kei.home->cpu : t->trap_cpu;
    unsigned best = 0;
    size_t best_load = SIZE_MAX;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        if (!q_usable(cpu)) continue;
        size_t load = q_resident(cpu);
        if (load < best_load || (load == best_load && cpu == prev)) { best = cpu; best_load = load; }
    }
    return best;
}

void sched_post_switch(void) {
    if (cpu_is_runner() || cpu_current_index() != 0) return;
    /* context_switchが旧taskのkspを書き、BSPが別stackへ移った後。 */
    struct task *t = bsp_handoff;
    if (!t || t == current_task) return;
    bsp_handoff = NULL;
    unsigned cpu = t->owner_cpu, home = t->kei.home ? (unsigned)t->kei.home->cpu : cpu;
    struct cpu_queue *q = &cpuq[cpu];
    q_lock2(home, cpu);
    enum kei_error e = kei_wake(&q->rq, &t->kei);
    bool kick = e == KEI_OK && (!q->rq.current || kei_decide(&q->rq).switch_needed);
    q_unlock2(home, cpu);
    if (e != KEI_OK) panic("kei: handoff of task %d to cpu %u failed %d", t->pid, cpu, e);
    STAT(handoffs);
    if (kick) {
        STAT(kicks);
        cpu_runner_wake_at(cpu);
    }
}

NORETURN void sched_ap_loop(void) {
    cli();
    sched_set_current(NULL);
    unsigned cpu = cpu_current_index();
    struct cpu_queue *q = &cpuq[cpu];
    cpu_runner_started();
    /* An idle AP keeps its tick: each one wakes it to steal from a busy queue (busiest_queue),
       and nothing else would tell it that work is waiting there. */
    for (;;) {
        cli();
        if (!q_resident(cpu)) cpu_park_check(); /* a sleep: nothing of ours is left here */
        if (cpu_worker_poll()) continue;
        bool returned = false;
        q_lock(q);
        struct task *t = ap_pick(q, &returned);
        if (t) q->stamp = tsc_hz >= 1000000 ? rdtsc() : 0;
        q_unlock(q);
        if (returned) ap_doorbell();
        if (!t) {
            /* IF=0の検査からsti;hltまでに来た起床IPIは保留され、hltを抜ける。 */
            unsigned src = busiest_queue(cpu);
            if (src && move_waiting(src, cpu)) { STAT(steals); continue; }
            __asm__ volatile("sti; hlt" : : : "memory");
            continue;
        }
        /* kernel mappingの変更はtlb_shootdownがこのCPUにも届けるので全消去は不要。
           user spaceの古いtagはvmm_switchがgen比較で破棄する。 */
        sched_set_current(t);
        tss_set_rsp0((uint64_t)t->kstack + KSTACK_PAGES * PAGE_SIZE);
        vmm_switch(t->pml4);
        wrmsr(0xC0000100, t->tls_base);
        fpu_restore(&t->fpu);
        context_switch(&runner_idle_sp[cpu], t->ksp);
        /* AP trapがこの固定stackへ戻った。tの文脈は保存済み。 */
        ap_retired(q, t);
    }
}

void sched_ap_interrupt(struct regs *r) {
    unsigned cpu = cpu_current_index();
    struct cpu_queue *q = &cpuq[cpu];
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
    /* Read-only identity queries need no BSP kernel continuation. */
    if (vector == 0x80 && (r->rax == SYS_CPU_INDEX || r->rax == SYS_THREAD_ID ||
                          r->rax == SYS_APP_SELF || r->rax == SYS_UPTIME)) {
        r->rax = r->rax == SYS_CPU_INDEX ? cpu : r->rax == SYS_THREAD_ID ? current_task->pid :
                 r->rax == SYS_APP_SELF ? task_process()->pid : uptime_ms();
        dispatch = false;
    }
    else if (vector==0x80 && net_ap_syscall(r)) dispatch=false;
    else if (vector==0x80 && address_ap_syscall(r)) dispatch=false;
    struct task *t = current_task;
    /* 継の判断：待機の先頭が継続幅を超えて先行したらAP内で切り替える。
       BSPを経由するのはkernel継続（syscall/fault）と空間の停止だけ。 */
    q_lock(q);
    ap_charge(q, t, vector == VEC_TIMER);
    unsigned reason = dispatch ? RET_DISPATCH : quiesce_match(t) ? RET_STOP :
                      kei_decide(&q->rq).switch_needed || cpu_worker_pending() ? RET_LOCAL : RET_NONE;
    q_unlock(q);
    if (reason == RET_NONE) return;
    q->reason = reason;
    prof_ap_retire(vector, r->rax);
    t->trap_cpu = cpu;
    /* CR2はfaultを起こしたCPUでcapture。BSPのread_cr2は決して代用しない。 */
    uint64_t remote_cr2 = vector == 14 ? read_cr2() : 0;
    fpu_save(&t->fpu);
    vmm_switch(kernel_pml4); /* PCIDならtagは残り、同じspaceへの再開でTLBが温かい */
    sched_set_current(NULL);
    context_switch(&t->ksp, runner_idle_sp[cpu]);
    /* APのキューから再開した（同じAPとは限らない）：launchが状態を整えた。 */
    if (cpu_is_runner()) return;
    /* BSPが受け取った（kernel継続、空間の停止、借用）。 */
    cpu_require_bsp();
    if (dispatch) isr_dispatch_remote(r, remote_cr2);
    else sched_user_return(r);
}

bool sched_runner_idle(unsigned cpu) {
    return cpu && cpu < CPU_MAX_COUNT && !q_resident(cpu);
}

/* 該当空間のuser threadをAPから全部外し、BSPのkqへ戻す。待機中は排他の中で
   抜き、走っているものは停止IPIで退かせて待つ。停止中は取得・起動もしない。 */
void sched_quiesce_space(uint64_t pml4) {
    cpu_require_bsp();
    if (!cpu_runner_ready()) return;
    ASSERT(!cpu_jobs_active()); /* worker callbackでmappingの変更/解放は禁止 */
    uint64_t flags = irq_save();
    quiesce_pml4 = pml4;
    __atomic_store_n(&quiescing, true, __ATOMIC_SEQ_CST);
    struct task *pulled = NULL, *running[CPU_MAX_COUNT] = { 0 };
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        struct cpu_queue *q = &cpuq[cpu];
        q_lock(q);
        for (;;) {
            struct kei_task *k = NULL;
            for (size_t i = 0; i < q->rq.count && !k; i++)
                if (quiesce_match(KEI_OWNER(q->rq.heap[i]))) k = q->rq.heap[i];
            if (!k) break;
            kei_sleep(&q->rq, k);
            KEI_OWNER(k)->rq_next = pulled;
            pulled = KEI_OWNER(k);
        }
        struct kei_task *cur = q->rq.current;
        if (cur && quiesce_match(KEI_OWNER(cur))) {
            if (cur->context_safe) {
                kei_sleep(&q->rq, cur);
                KEI_OWNER(cur)->rq_next = pulled;
                pulled = KEI_OWNER(cur);
            } else {
                running[cpu] = KEI_OWNER(cur);
            }
        }
        q_unlock(q);
        if (running[cpu]) cpu_runner_wake_at(cpu);
    }
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++) {
        struct task *t = running[cpu];
        if (!t) continue;
        struct cpu_queue *q = &cpuq[cpu];
        uint64_t since = rdtsc(), spins = 0;
        for (;;) {
            q_lock(q);
            struct kei_task *cur = q->rq.current;
            bool done = cur != &t->kei;
            if (!done && cur->context_safe) { /* AP内の切替で退いたまま */
                kei_sleep(&q->rq, cur);
                t->rq_next = pulled;
                pulled = t;
                done = true;
            }
            q_unlock(q);
            if (done) break;
            cpu_runner_check();
            if (cpu_ap_answer_late(since, &spins))
                panic("smp: user retire timeout; task/space/context retained");
            pause();
        }
    }
    while (pulled) {
        struct task *t = pulled;
        pulled = t->rq_next;
        t->rq_next = NULL;
        t->owner_cpu = 0;
        sched_make_ready(t);
    }
    collect_runner();
    __atomic_store_n(&quiescing, false, __ATOMIC_RELEASE);
    /* IRQ待ちにBSPのqueue/allocator lockをAPへ要求しない。 */
    irq_restore(flags);
}

void sched_get_stats(struct sched_stats *out) {
    out->handoffs = __atomic_load_n(&stats.handoffs, __ATOMIC_RELAXED);
    out->local_switches = __atomic_load_n(&stats.local_switches, __ATOMIC_RELAXED);
    out->dispatch_retires = __atomic_load_n(&stats.dispatch_retires, __ATOMIC_RELAXED);
    out->stop_retires = __atomic_load_n(&stats.stop_retires, __ATOMIC_RELAXED);
    out->steals = __atomic_load_n(&stats.steals, __ATOMIC_RELAXED);
    out->balances = __atomic_load_n(&stats.balances, __ATOMIC_RELAXED);
    out->kicks = __atomic_load_n(&stats.kicks, __ATOMIC_RELAXED);
    out->borrows = __atomic_load_n(&stats.borrows, __ATOMIC_RELAXED);
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
    kei_retention_v = kei_retention_from_cost(KEI_SWITCH_COST_NS, KEI_OVERHEAD_PPM, 500000, 4000000);
    kprintf("sched: kei (per-AP queues, retention %lluus); BSP borrow slice %uus\n",
            (unsigned long long)(kei_retention_v / 1000), BSP_USER_SLICE_US);
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
    if (next_pid > INT32_MAX) {
        irq_restore(f);
        vfree(t->kstack, KSTACK_PAGES);
        kfree(t);
        return NULL;
    }
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
    if (t->kei_tracked) kei_users--; /* 終了済み：どのキューにもいない */
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
    if (prev->owner_cpu) ASSERT(prev == bsp_handoff); /* post_switchでAPのキューへ入る */
    if (prev->state != TASK_RUNNING) prev->bsp_borrow = false;
    if (prev->state == TASK_RUNNING && prev != idle_task && !prev->owner_cpu) {
        rq_push(prev);
    }
    struct task *next = rq_pop();
    if (!next) next = bsp_borrow_task(); /* kernelの仕事がない：待っているring3を借りる */
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
    if (queue_capacity && now - balance_ms >= BALANCE_MS) {
        balance_ms = now;
        ap_balance();
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

/* BSP idle, interrupts off: sched_tick has nothing to do until a timeout. Nothing is ready, and
   no AP runs or holds an app task (their uptime_ms reads need the tick, and so does ap_balance). */
bool sched_bsp_quiet(void) {
    if (current_task != idle_task || need_resched || kq.head ||
        __atomic_load_n(&runner_return_pending, __ATOMIC_ACQUIRE))
        return false;
    for (unsigned cpu = 1; cpu < CPU_MAX_COUNT; cpu++)
        if (q_resident(cpu) || __atomic_load_n(&cpuq[cpu].returned, __ATOMIC_RELAXED)) return false;
    return true;
}

/* After S3 the TSC may have started again from 0: the BSP's running stamp starts over. */
void sched_clock_resume(void) {
    bsp_stamp = run_stamp();
    bsp_stamp_tsc = tsc_hz >= 1000000;
}

uint64_t sched_next_wake(void) {
    uint64_t next = UINT64_MAX;
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->state == TASK_BLOCKED && t->wake_at && t->wake_at < next) next = t->wake_at;
    return next;
}

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatchはstiして返る。所有publicationからiretまでIRQを閉じる。 */
    cli();
    for (;;) {
    if (current_task->killed) task_exit(current_task->kill_code);
    struct task *t = current_task;
    if (!cpu_runner_ready() || !t->is_user) {
        if (need_resched) schedule();
        if (current_task->killed) task_exit(current_task->kill_code);
        return;
    }
    collect_runner();
    /* 借りたring3は、kernelの仕事も空いたAPもない間BSPで続ける。 */
    if (t->bsp_borrow && !need_resched) return;
    bsp_account(); /* BSP上の最後の量を確定してから会計をAPへ渡す */
    if (!t->kei_tracked) {
        kei_task_init(&t->kei, (uint64_t)t->pid, KEI_WEIGHT_ONE, NULL);
        t->kei_tracked = true;
        kei_users++;
    }
    unsigned cpu = queues_reserve(kei_users) ? place(t) : 0;
    if (!cpu) {
        /* キューを用意できない：BSPで続け、次の境界でもう一度移す。 */
        t->bsp_borrow = true;
        if (need_resched) schedule();
        continue;
    }
    ASSERT(!(read_rflags() & 0x200) && !cpu_jobs_active());
    t->bsp_borrow = false;
    t->owner_cpu = cpu;
    bsp_handoff = t;
    schedule();
    /* APで再開した：shared scheduler/kernelを呼ばずiretへ。 */
    if (cpu_is_runner()) return;
    /* BSPが借りて再開した：もう一度確かめる。 */
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
        if (p->is_thread && p->process == owner && p->state != TASK_ZOMBIE) count++; /* exited, not yet joined: no longer a running thread */
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
    if (vmm_user_reserve(t->pml4, t->user_stack, t->user_stack_size, VM_W) < 0) { /* pages on first touch */
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
    if (!phys && vmm_demand_fault(t->pml4, (uint64_t)address, false) > 0) phys = vmm_translate(t->pml4, (uint64_t)address);
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
    if (!phys && vmm_demand_fault(current_task->pml4, (uint64_t)address, false) > 0)
        phys = vmm_translate(current_task->pml4, (uint64_t)address);
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

NORETURN void task_exit_app(int code) {
    cpu_require_bsp();
    cli();
    struct task *owner = task_process();
    if (current_task->is_thread && !owner->exiting) {
        /* Ending the app from one of its threads (ISO C exit, or a fault that
           may abandon a pool descriptor or a shared mutex) cannot be done by
           zombifying just this helper: abort/wake the owner and every sibling
           so the app ends with this code and no user continuation waits for a
           dead holder. task_exit then joins them before destroying their
           shared mappings. */
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
        /* The thread never returns to ring 3, so its user stack is dead now. An
           unjoined (or detached) zombie keeps only its kernel stack and task record
           instead of 256 KiB of address space and the pages it touched. */
        struct task *owner = t->process;
        if (t->user_stack && owner && !owner->exiting && owner->pml4 == t->pml4 && t->pml4 != kernel_pml4) {
            uint64_t stack = t->user_stack, size = t->user_stack_size;
            t->user_stack = t->user_stack_size = 0;
            vmm_user_free(t->pml4, stack, size);
            mmap_hole_add(owner, stack - PAGE_SIZE, size + 2 * PAGE_SIZE);
        }
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
    /* PCI claims first, whatever their handle numbers: a driver's device is stopped (and seen
       to have no DMA in flight) before the DMA objects among its other handles can free RAM. */
    for (int claims = 1; claims >= 0; claims--)
        for (size_t i = 0; i < t->fd_capacity; i++) {
            if (t->fds[i] && (!claims || pci_user_is_claim(t->fds[i]))) {
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

/* BSP IRQ guard held. */
static void kill_locked(struct task *t) {
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
}

int task_kill(int pid) {
    uint64_t f = irq_save();
    struct task *t = task_find(pid);
    if (!t || t == idle_task) { irq_restore(f); return -N_ERR_NO_APP; }
    if (!t->is_user) { irq_restore(f); return -N_ERR_DENIED; }
    kill_locked(t);
    irq_restore(f);
    return 0;
}

int task_stop_apps(void) {
    int alive = 0;
    uint64_t f = irq_save();
    for (struct task *t = task_list; t; t = t->all_next) {
        if (!t->is_user || t->state == TASK_ZOMBIE) continue;
        alive++;
        if (!t->killed) kill_locked(t);
    }
    irq_restore(f);
    return alive;
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
    if (!file) return;
    /* A message channel has no byte write: the text goes as one message. */
    if (message_channel_is(file)) message_channel_post(file, buf, (size_t)n, (uint64_t)owner->pid);
    else vfs_write(file, buf, n);
    vfs_close(file);
}
