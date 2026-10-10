#pragma once
#include "kernel.h"
#include "arch/fpu.h"

struct regs;
struct file;

#define MAX_FDS 32
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
    struct task *process;     /* shared VM / descriptors / cwd / brk owner */
    uint64_t user_stack, user_stack_size;
    volatile uint32_t *wait_address;
    unsigned trap_cpu;
    uint64_t tls_base;
    uint64_t clock_page;
    uint64_t ksp;
    uint8_t *kstack;
    uint64_t pml4;
    struct file *fds[MAX_FDS];
    char cwd[256];
    struct task *parent;
    int exit_code;
    int kill_code;          /* ordinary kill=130; fatal sibling keeps exception status */
    uint64_t wake_at;        /* ms; 0 = no timeout */
    bool timed_out;
    struct task *rq_next;     /* ready queue */
    struct task *all_next;    /* global task list */
    struct task *wq_next;     /* wait queue */
    struct wait_queue *wq;
    struct wait_queue child_wait;
    uint64_t brk_base, brk;
    uint64_t mmap_next;
    uint64_t cpu_ms;
    uint64_t start_ms;
    unsigned sched_level;     /* 0=interactive, 3=CPU-bound; ready/wait policy is BSP-owned */
    uint32_t sched_used_us;   /* cumulative allotment: yield/short sleep does not refund it */
    uint32_t slice_us;        /* remaining quantum, preserved across early yield/block */
    uint16_t cpu_subms_us;
    uint64_t sched_epoch;     /* lazy boost for blocked and AP-owned tasks */
};

struct task **sched_current_slot(void);
volatile bool *sched_resched_slot(void);
#define current_task (*sched_current_slot())
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
int task_wait_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms);
int task_wake_address(volatile uint32_t *address, unsigned count);
NORETURN void task_exit(int code);
NORETURN void task_fault_exit(int code); /* fatal user exception aborts shared process */
struct task *task_find(int pid);
int task_kill(int pid);
int task_wait(int pid, int *status, bool nohang);
void task_printf_stderr(struct task *t, const char *fmt, ...);

/* wait queues; call with interrupts disabled for race-free condition checks */
void wq_wait(struct wait_queue *wq);
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms); /* returns false on timeout */
void wq_wake_all(struct wait_queue *wq);
void sleep_ms(uint64_t ms);

/* a global "something changed" queue used by poll() */
extern struct wait_queue poll_wq;
static inline void poll_notify(void) { wq_wake_all(&poll_wq); }
