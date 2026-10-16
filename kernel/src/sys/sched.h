#pragma once
#include "kernel.h"
#include "arch/fpu.h"
#include "arch/smp.h"

struct regs;
struct file;

#define MAX_FDS 32 /* initial inline capacity, never a descriptor limit */
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
    unsigned rq_cpu;        /* BSP-owned per-CPU run queue / load balancing hint */
    unsigned owner_cpu;       /* 0=BSP。AP retire ACK前はqueue/free禁止 */
    bool is_thread;
    bool exiting;
    bool joining;
    struct task *process;     /* shared VM / handles owner */
    uint64_t user_stack, user_stack_size;
    uint64_t wait_phys, wait_seq; /* address wait: physical key, FIFO order */
    unsigned trap_cpu;
    uint64_t tls_base;
    uint64_t clock_page;
    uint64_t ksp;
    uint8_t *kstack;
    uint64_t pml4;
    struct file *initial_fds[MAX_FDS];
    struct file **fds;
    struct file *net_setup_cancel_file; /* thread-owned strong ref; never inherited */
    size_t fd_capacity,fd_hint;
    struct task *parent;
    struct vnode *app;        /* its handle record reports the end */
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
    struct shared_mapping *shared_mappings; /* native FD-backed page references */
    uint64_t cpu_ms;
    uint64_t start_ms;
    unsigned sched_level;     /* 0=interactive, 3=CPU-bound; ready/wait policy is BSP-owned */
    uint32_t sched_used_us;   /* cumulative allotment: yield/short sleep does not refund it */
    uint32_t slice_us;        /* remaining quantum, preserved across early yield/block */
    uint16_t cpu_subms_us;
    uint64_t sched_epoch;     /* lazy boost for blocked and AP-owned tasks */
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
void sched_user_return(struct regs *r);
void sched_post_switch(void); /* context_switch: 旧taskstackを離れた後だけpublication */
NORETURN void sched_ap_loop(void);
void sched_ap_interrupt(struct regs *r);
void sched_quiesce_space(uint64_t pml4); /* 0=shared kernel変更、他は該当user space */
void sched_pin_space(uint64_t pml4);
int task_thread_create(uint64_t entry, uint64_t function, uint64_t argument);
int task_thread_join(int tid);
/* wide: compare the 8-byte word at address (wasm memory.atomic.wait64). */
int task_wait_address(volatile void *address, uint64_t expected, bool wide, unsigned timeout_ms);
int task_wake_address(volatile uint32_t *address, unsigned count);
/* Asynchronous address waiters. A watch joins the same physical-address FIFO
 * as blocked tasks; a notify that selects it moves its token to the queue's
 * pollable ready list. Removing reports 1 while still waiting, 2 after it was
 * notified (the notifier already counted it), 0 for an unknown token. */
struct vnode *address_watch_queue_new(void);
int address_watch_add(struct vnode *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
int address_watch_remove(struct vnode *queue, uint64_t token);
size_t address_watch_take(struct vnode *queue, uint64_t *tokens, size_t max);
NORETURN void task_exit(int code);
NORETURN void task_fault_exit(int code); /* fatal user exception aborts shared process */
struct task *task_find(int pid);
int task_kill(int pid);
int task_wait(int pid, int *status, bool nohang);
void task_printf_stderr(struct task *t, const char *fmt, ...);
/* Descriptor representation helpers; caller holds a BSP IRQ guard. File
 * references transfer only on successful installation. Explicit indices may
 * grow to the int ABI's representable boundary, subject to real allocation. */
int task_fd_reserve(struct task *owner,size_t minimum);
int task_fd_find(struct task *owner);
int task_fd_alloc(struct task *owner,struct file *file);

/* wait queues; call with interrupts disabled for race-free condition checks */
void wq_wait(struct wait_queue *wq);
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms); /* returns false on timeout */
void wq_wake_all(struct wait_queue *wq);
void sleep_ms(uint64_t ms);
