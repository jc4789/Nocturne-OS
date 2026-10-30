#pragma once
#include "kernel.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "sys/kei.h"
#include "sys/syscall.h"

struct regs;
struct handle;
struct object;

#define INITIAL_HANDLES 32 /* the inline table's size, never a handle limit */
#define KSTACK_PAGES 8

/* TASK_RUNNING: it runs, or may run (its CPU's queue holds it); TASK_READY is what apps are told
   of one waiting in a queue (n_procinfo). */
enum task_state { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE };

struct task;
/* A blocked task is in exactly one wait queue (a deadline alone has one of its own); whoever takes
   it out under the queue's lock is its one waker. The queue's lock comes before a run queue's. A
   waiter whose condition another CPU changes reads it under a lock that waker also holds while it
   wakes, or loses the wakeup between its check and its sleep. */
struct wait_queue {
    struct task *head;
    struct spinlock lock;
};

struct task {
    struct fpu_state fpu;
    int pid;
    char name[32];
    enum task_state state;    /* changes under its run queue's lock (and a wait queue's to block) */
    bool is_user;
    bool killed;
    bool is_idle;             /* a CPU's idle task: never queued, never asleep */
    bool is_thread;
    bool exiting;
    bool joining;
    bool detached;            /* thread: freed when it has ended instead of joined */
    unsigned cpu;             /* the run queue whose lock guards kei (sched.c) */
    struct task *process;     /* shared VM / handles owner */
    uint64_t user_stack, user_stack_size;
    uint64_t wait_phys, wait_seq; /* address wait: physical key, FIFO order */
    uint64_t tls_base;
    uint64_t clock_page;      /* a process's mapping of the hypervisor clock page: set once under space's lock, read atomically */
    uint64_t ksp;
    uint8_t *kstack;
    uint64_t pml4;
    struct handle *initial_handles[INITIAL_HANDLES];
    struct handle **handles;          /* a process's table, under handles_lock (sys/object.c) */
    struct handle *net_setup_cancel; /* thread-owned strong ref; never inherited */
    size_t handle_capacity, handle_hint;
    struct spinlock handles_lock;
    struct task *parent;
    struct object *app;       /* its app record reports the end */
    bool app_launched;        /* by n_app_launch: no parent collects it */
    int exit_code;
    int kill_code;          /* ordinary kill=130; fatal sibling keeps exception status */
    uint64_t wake_at;        /* ms; 0 = no timeout */
    bool timed_out;
    struct task *dead_next;   /* the reaper's list */
    struct task *all_next;    /* global task list (task_lock) */
    struct task *wq_next;     /* wait queue */
    struct wait_queue *wq;
    struct wait_queue child_wait;
    /* A process's address-space layout (sys/syscall.c). space's lock guards the reservation
       regions' frontiers (mmap_next, low_next) and holes, clock_page, the ranges its threads are
       changing (space_ops) and space_ended; space holds the threads waiting for such a range. It
       comes before shared_lock (syscall.c), the page tables' locks and pmm's, and no two
       processes' are held at once. shared_mappings changes with both space's lock and shared_lock
       held: either is enough to read it. low_base is set before the process runs. */
    struct wait_queue space;
    struct space_op *space_ops;
    bool space_ended;         /* its mappings went at its end: nothing is reserved or freed again */
    uint64_t mmap_next;
    struct mmap_hole *mmap_holes; /* freed anonymous ranges below mmap_next, by address */
    /* The low region [low_base, USER_LOW_TOP): native memory reserved with
       N_MEMORY_LOW for code that holds 32-bit absolute addresses. */
    uint64_t low_base, low_next;
    struct mmap_hole *low_holes;
    struct shared_mapping *shared_mappings; /* shared pages this app has mapped, also being built */
    uint64_t cpu_ms;
    uint64_t start_ms;
    uint16_t cpu_subms_us;
    struct kei_task kei;      /* its run queue's lock guards it */
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
/* Every task, newest first. task_lock guards the list and the links of a task's family (parent,
   process, app); a task leaves the list, and is freed, only under it. It comes before a wait
   queue's lock. */
extern struct task *task_list;
extern struct spinlock task_lock;
static inline struct task *task_process(void) {
    return current_task->process ? current_task->process : current_task;
}

/* Each CPU has a run queue (kei, sys/kei.c) and an idle task. A task runs on the CPU whose queue
   holds it; a CPU that runs out of tasks takes waiting ones from a busier queue, and a woken one
   goes to an idle CPU first. Kernel code is not preempted: a CPU switches tasks when its task
   blocks, yields or goes back to ring 3. */
void sched_init(void);                 /* CPU 0, at boot: its queue, the boot context as its idle task */
bool sched_cpu_prepare(unsigned cpu);  /* CPU 0, before that CPU starts: its queue and idle task */
NORETURN void sched_cpu_start(void);   /* a CPU starting: its idle task from here on */
NORETURN void sched_idle(void);        /* the idle task's loop (CPU 0's after boot) */
bool sched_cpu_idle(unsigned cpu);     /* running its idle task now (a hint) */
bool sched_cpu_empty(unsigned cpu);    /* its queue holds no task (a hint) */
void sched_cpu_leave(unsigned cpu);    /* for a sleep: its tasks go to CPU 0, and none come */
struct task *task_alloc(const char *name);
void task_discard(struct task *t); /* a task_alloc'd task that never ran: unlinked and freed */
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg);
/* Any CPU, any context: a new task, or a blocked one its waker took out of its wait queue. */
void sched_make_ready(struct task *t);
void schedule(void);
void sched_yield(void);
void sched_tick(void);          /* each CPU's timer interrupt */
void sched_timeouts(void);      /* CPU 0's timer interrupt: the deadlines that have come */
uint64_t sched_next_wake(void); /* the earliest timeout (uptime_ms), UINT64_MAX: none */
void sched_clock_resume(void);  /* after S3 (the TSC may have restarted), interrupts off */
void sched_user_return(struct regs *r);
void sched_post_switch(void);   /* context_switch, on the new task's stack */
struct sched_stats { uint64_t switches, wakes, moved_wakes, steals, balances, kicks, yields; };
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
/* Reading only: notified tokens wait to be taken (one notified meanwhile may be missed). */
bool address_watch_pending(struct object *queue);
NORETURN void task_exit(int code);
NORETURN void task_exit_app(int code); /* SYS_EXIT or a fatal user exception: the whole app ends, from any of its threads */
/* task_lock held: the live task with that ID, or NULL. */
struct task *task_find(int pid);
int task_kill(int pid);
/* Every live app task below pid (and pid itself with include_self) is killed: how many. */
int task_kill_tree(int pid, bool include_self);
int task_stop_apps(void); /* power change: kill every app task; returns how many have not ended yet */
int task_wait(int pid, int *status, bool nohang);
void task_printf_stderr(struct task *t, const char *fmt, ...);
/* Wait queues. A condition another CPU changes is looked at under the lock its changer holds,
   then wq_wait_locked; plain wq_wait fits a waiter that looks again after each wake anyway. */
void wq_wait(struct wait_queue *wq);
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms); /* returns false on timeout */
/* For a condition another CPU changes without a lock this waiter can hold: sleep on wq unless
   *word (which the waker moves on before it wakes wq) no longer reads seen, the value read before
   the condition was checked. ms 0: no limit; false on timeout. */
bool wq_wait_unless(struct wait_queue *wq, const uint64_t *word, uint64_t seen, uint64_t ms);
/* Interrupts off, held taken: sleep on wq, held given back once enrolled (a waker that takes held
   to change the condition, then wakes wq, finds the task there). Returns without held. held may be
   wq's own lock. */
void wq_wait_locked(struct wait_queue *wq, struct spinlock *held);
bool wq_wait_locked_timeout(struct wait_queue *wq, struct spinlock *held, uint64_t ms); /* false on timeout */
void wq_wake_all(struct wait_queue *wq);
/* A short wait while polling a device or a flag: asleep for up to ms (at least 1). The boot task
   (later the idle task) has no stack of its own to sleep on: it halts until the next interrupt
   instead and lets the other tasks run. */
void nap_ms(uint64_t ms);
void sleep_ms(uint64_t ms);
/* A sleeping lock: state that stays borrowed across allocations, copies, waits or disk transfers.
   The holder may take it again. Interrupt handlers never take one. The boot task, which cannot
   sleep, lets the holder run until it is free. */
struct mutex {
    struct task *owner;
    unsigned depth;
    struct wait_queue wait; /* its lock guards owner and depth */
};
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(struct mutex *m); /* by the current task */
/* Holding m (taken once): sleeps on wq with m let go, then takes m again. Whoever changes what the
   sleeper waits for holds m while it does, and wakes wq after: wq's lock is taken before m is let
   go, so a wake on another CPU finds the sleeper in wq. (wq's lock comes before m's own; nothing
   takes them the other way.) */
void mutex_wait(struct mutex *m, struct wait_queue *wq);
