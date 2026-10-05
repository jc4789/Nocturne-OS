#pragma once
#include "kernel.h"

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
    uint8_t fpu[512] __attribute__((aligned(16)));
    int pid;
    char name[32];
    enum task_state state;
    bool is_user;
    bool killed;
    bool on_rq;
    uint64_t ksp;
    uint8_t *kstack;
    uint64_t pml4;
    struct file *fds[MAX_FDS];
    char cwd[256];
    struct task *parent;
    int exit_code;
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
    int slice;
};

extern struct task *current_task;
extern struct task *task_list;
extern volatile bool need_resched;

void sched_init(void);
struct task *task_alloc(const char *name);
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg);
void sched_make_ready(struct task *t);
void schedule(void);
void sched_yield(void);
void sched_tick(void);
void sched_user_return(struct regs *r);
NORETURN void task_exit(int code);
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
