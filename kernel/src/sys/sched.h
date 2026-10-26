#pragma once
#include "kernel.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "sys/kei.h"
#include "sys/syscall.h"

struct regs;
struct handle;
struct object;

#define INITIAL_HANDLES 32 /* the inline table's size, never a handle limit */
#define KSTACK_PAGES 8

enum task_state { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE };

struct task;
struct wait_queue {
    struct task *head;
};

struct task {
    struct fpu_state fpu;
    int pid;
    char name[32];
    enum task_state state;
    bool is_user;
    bool killed;
    bool on_rq;
    unsigned owner_cpu;       /* 0=BSP。それ以外はそのAPのキュー所有（返却まで触れない） */
    bool is_thread;
    bool exiting;
    bool joining;
    bool detached;            /* thread: freed when it has ended instead of joined */
    struct task *process;     /* shared VM / handles owner */
    uint64_t user_stack, user_stack_size;
    uint64_t wait_phys, wait_seq; /* address wait: physical key, FIFO order */
    uint8_t address_park;     /* APで寝るaddress待ちの段階（sched.cのPARK_*、address_lockの下） */
    bool address_woken;       /* 寝たaddress待ちが起こされた：再開でsyscallを0で終える */
    bool address_timed_out;   /* その起こしは期限切れ（BSPの拍）：時間切れで終える */
    uint64_t park_deadline;   /* 有限の寝るaddress待ちの期限（uptime_ms）。0は無期限 */
    uint64_t wait_until;      /* APで寝た事象待ちと眠りを、やり直しでも守る最初の期限（uptime_ms、UINT64_MAXは無期限）。0はやり直しでない */
    struct task *park_next;   /* APで寝るaddress待ちの一覧（到着順） */
    unsigned trap_cpu;
    uint64_t tls_base;
    uint64_t clock_page;
    uint64_t ksp;
    uint8_t *kstack;
    uint64_t pml4;
    struct handle *initial_handles[INITIAL_HANDLES];
    struct handle **handles;
    struct handle *net_setup_cancel; /* thread-owned strong ref; never inherited */
    size_t handle_capacity, handle_hint;
    struct task *parent;
    struct object *app;       /* its app record reports the end */
    bool app_launched;        /* by n_app_launch: no parent collects it */
    int exit_code;
    int kill_code;          /* ordinary kill=130; fatal sibling keeps exception status */
    uint64_t wake_at;        /* ms; 0 = no timeout */
    bool timed_out;
    struct task *rq_next;     /* ready queue */
    struct task *all_next;    /* global task list */
    struct task *wq_next;     /* wait queue */
    struct wait_queue *wq;
    struct wait_queue child_wait;
    uint64_t mmap_next;
    struct mmap_hole *mmap_holes; /* freed anonymous ranges below mmap_next, by address */
    /* The low region [low_base, USER_LOW_TOP): native memory reserved with
       N_MEMORY_LOW for code that holds 32-bit absolute addresses. */
    uint64_t low_base, low_next;
    struct mmap_hole *low_holes;
    struct shared_mapping *shared_mappings; /* shared pages this app has mapped */
    uint64_t cpu_ms;
    uint64_t start_ms;
    uint16_t cpu_subms_us;
    struct kei_task kei;      /* 継の会計。APのキューにいる間はそのキューの排他下 */
    bool kei_tracked;
    bool bsp_borrow;          /* APのキューからBSPが借りてring3を走らせている */
    uint64_t prof_stamp[3];   /* prof：BSPへ回した時刻・BSPが受けた時刻・APへ渡した時刻（TSC） */
    uint32_t prof_number;     /* prof：回した syscall の番号 */
};

static inline struct task *sched_current(void) {
    struct task *task;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(task)
                     : "i"(__builtin_offsetof(struct cpu_local, task)));
    return task;
}
static inline void sched_set_current(struct task *task) {
    __asm__ volatile("movq %0, %%gs:%c1" : : "r"(task),
                     "i"(__builtin_offsetof(struct cpu_local, task)) : "memory");
}
static inline volatile bool *sched_resched_slot(void) { return &cpu_local_get()->resched; }
#define current_task sched_current()
#define need_resched (*sched_resched_slot())
extern struct task *task_list;
static inline struct task *task_process(void) {
    return current_task->process ? current_task->process : current_task;
}

void sched_init(void);
struct task *task_alloc(const char *name);
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg);
void sched_make_ready(struct task *t);
void schedule(void);
void sched_yield(void);
void sched_tick(void);
bool sched_bsp_quiet(void);      /* tickless idle (dev/timer.c): nothing to run, no app on an AP */
uint64_t sched_next_wake(void);  /* the earliest timeout (uptime_ms), UINT64_MAX: none */
void sched_clock_resume(void); /* after S3 (the TSC may have restarted), interrupts off */
void sched_user_return(struct regs *r);
void sched_post_switch(void); /* context_switch: 旧taskstackを離れた後だけpublication */
NORETURN void sched_ap_loop(void);
void sched_ap_interrupt(struct regs *r);
void sched_quiesce_space(uint64_t pml4); /* 0=shared kernel変更、他は該当user space */
bool sched_runner_idle(unsigned cpu);    /* BSP: そのAPにuser taskが無い（kernel jobに使える） */
struct sched_stats { uint64_t handoffs, local_switches, dispatch_retires, stop_retires, steals, balances, kicks, borrows, parks, yields; };
void sched_get_stats(struct sched_stats *out);
/* stack_bytes 0 is THREAD_STACK_DEFAULT; others are rounded up to whole pages. */
#define THREAD_STACK_DEFAULT (256 * 1024)
int task_thread_create(uint64_t entry, uint64_t function, uint64_t argument, uint64_t stack_bytes);
int task_thread_join(int tid);
/* No one will join the thread: it is freed once it has ended. */
int task_thread_detach(int tid);
/* wide: compare the 8-byte word at address (wasm memory.atomic.wait64). */
int task_wait_address(volatile void *address, uint64_t expected, bool wide, unsigned timeout_ms);
int task_wake_address(volatile uint32_t *address, unsigned count);
/* Asynchronous address waiters. A watch joins the same physical-address FIFO
 * as blocked tasks; a notify that selects it moves its token to the queue's
 * pollable ready list. Removing reports 1 while still waiting, 2 after it was
 * notified (the notifier already counted it), 0 for an unknown token. */
struct object *address_watch_queue_new(void);
int address_watch_add(struct object *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
int address_watch_remove(struct object *queue, uint64_t token);
size_t address_watch_take(struct object *queue, uint64_t *tokens, size_t max);
/* Any CPU, reading only: notified tokens wait to be taken (one notified meanwhile may be missed). */
bool address_watch_pending(struct object *queue);
/* A kernel word that changes when a wait must look again (an object_wait's generation). On an AP,
   in an AP read section that keeps the word allocated: park the calling task on it while it still
   reads seen, until a wake or deadline (uptime_ms; UINT64_MAX: none). False: it moved on, and
   nothing was enrolled. A waker on any CPU moves the word on first, then wakes every task parked
   on it. */
bool address_park_word(const uint64_t *word, uint64_t seen, uint64_t deadline);
void address_park_wake_word(const uint64_t *word);
/* SYS_SLEEP on an AP (syscall_ap): the calling task parks until at least ms have passed. */
enum ap_syscall sleep_ap(uint64_t ms);
NORETURN void task_exit(int code);
NORETURN void task_exit_app(int code); /* SYS_EXIT or a fatal user exception: the whole app ends, from any of its threads */
struct task *task_find(int pid);
int task_kill(int pid);
int task_stop_apps(void); /* power change: kill every app task; returns how many have not ended yet */
int task_wait(int pid, int *status, bool nohang);
void task_printf_stderr(struct task *t, const char *fmt, ...);
/* wait queues; call with interrupts disabled for race-free condition checks */
void wq_wait(struct wait_queue *wq);
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms); /* returns false on timeout */
void wq_wake_all(struct wait_queue *wq);
/* A driver waiting for its device: asleep on wq until *kick is set (its interrupt handler sets it,
   then wakes wq) or for up to ms; *kick is cleared. wq and kick may be NULL: just the time. The
   boot task (later the idle task) has no stack of its own to sleep on: it halts until the next
   interrupt instead and lets the other kernel threads run. */
void wq_nap(struct wait_queue *wq, volatile bool *kick, uint64_t ms);
void sleep_ms(uint64_t ms);
/* SYS_SLEEP on the BSP: until the deadline a sleep begun on an AP set (wait_until), else ms. */
void sleep_resume(uint64_t ms);
/* A sleeping lock for kernel code on the BSP, which a timer may preempt: state that stays
   borrowed across allocations, copies or disk transfers. The holder may take it again. Interrupt
   handlers never take one. The boot task, which cannot sleep, lets the holder run until it is
   free. */
struct mutex {
    struct task *owner;
    unsigned depth;
    struct wait_queue wait;
};
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
