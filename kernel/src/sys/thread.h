/* Threads: what a CPU runs (sys/thread.c). An app's threads belong to its process (sys/process.h);
   a kernel thread belongs to none. */
#pragma once
#include "kernel.h"
#include "arch/percpu.h"
#include "arch/spin.h"
#include "lib/idtable.h"
#include "sys/sched.h"
#include "sys/wait.h"

struct fpu_state;
struct handle;
struct process;
struct regs;

#define KSTACK_PAGES 8
/* An app thread's stack when it asks for no size (thread_create's stack_bytes 0). */
#define THREAD_STACK_DEFAULT (256 * 1024)

struct thread {
    struct fpu_state *fpu;     /* its extended state, in the same allocation (sys/thread.c) */
    int tid;                   /* positive; a process's main thread's is the process's pid */
    char name[32];
    /* Its app's process, NULL for a kernel thread. No kernel thread has handles, an address space,
       a family or an app record of its own: the kernel's threads (irq threads, the power thread,
       vmalloc's, the log readers...) work on the kernel's own state. */
    struct process *process;
    bool killed;               /* it is to end: the killable waits look at it, and so does the way to ring 3 */
    int kill_code;             /* its end's code once killed: 130, or the status of a fatal sibling */
    bool joining;              /* a sibling waits for its end (thread_join; thread_lock) */
    bool detached;             /* no one joins it: its record goes when it ends (thread_lock) */
    bool ended;                /* its CPU has left it for good after its end (thread_left; thread_lock) */
    uint64_t ksp;
    uint8_t *kstack;           /* KSTACK_PAGES */
    uint64_t fs_base, gs_base; /* the app's, while it is not on a CPU (schedule) */
    /* A process's other thread's stack in its mmap region; 0 for the main thread's, which is the
       stack region below USER_STACK_TOP. */
    uint64_t user_stack, user_stack_size;
    /* A channel of the app's whose peer closing cancels this thread's network setup waits
       (net/net.c, SYS_NET_SETUP_CANCEL_CHANNEL): a reference of the thread's own, closed at its end. */
    struct handle *net_setup_cancel;
    uint64_t start_ns;         /* clock_now_ns when it was made */
    struct sched_entity sched; /* sys/sched.h */
    struct thread_wait wait;   /* sys/wait.h */
    struct id_link id;         /* in the registry by tid (thread_lock) */
    struct thread *all_next, **all_prev;         /* thread_list (thread_lock) */
    struct thread *process_next, **process_prev; /* its process's threads list (thread_lock) */
};

static inline struct thread *thread_current(void) { return this_cpu(sched_thread); }

/* The registry of threads: every thread whose record exists, by tid and in thread_list (the newest
   first). thread_lock guards it, each process's list of its other threads and the family links of
   processes (sys/process.h); a thread leaves them, and is freed, only under it. It comes before a
   wait queue's lock. Each operation under it is O(1) or walks one process's threads or children,
   but for the reports that read every thread (the app list, the memory report). */
extern struct spinlock thread_lock;
extern struct thread *thread_list;
/* thread_lock held: the thread with that id (an ended one too, until its record goes), or NULL. */
struct thread *thread_find(int tid);
size_t thread_count(void); /* threads listed now (a hint) */

/* A thread not runnable yet: its record, kernel stack, id, place in the registry and part in the
   scheduler (sched_thread_add). p: its process, whose main thread it is while p has none, one of
   its other threads (in p->threads) after. NULL without memory. */
struct thread *thread_new(const char *name, struct process *p);
/* A CPU's idle thread (sys/sched.c), its kernel stack on node: never queued, never freed. */
struct thread *thread_new_idle(const char *name, unsigned node);
/* A new app thread's first switch enters ring 3 with frame (user_trampoline). */
void thread_enter_user(struct thread *t, const struct regs *frame);
/* An ended thread no one will join, out of its process's list, or a main thread never made ready:
   once its CPU has left it for good, its id, kernel stack and record go. */
void thread_free(struct thread *t);
/* An ended thread its CPU has just left for good (sys/sched.c, after the switch away, interrupts off
   and no lock held): a kernel thread, or one no one joins, is freed here; a process's main thread
   lets go of its process; another is marked ended, for its joiner. */
void thread_left(struct thread *t);

struct thread *kthread_create(const char *name, void (*fn)(void *), void *arg);
/* A kernel thread that runs on that CPU alone: a sequence the CPU has to run itself (the power
   transitions, on the first CPU, which the firmware wakes from S3). */
struct thread *kthread_create_on(const char *name, void (*fn)(void *), void *arg, unsigned cpu);
/* An app thread of the calling process (SYS_THREAD_CREATE): its tid. stack_bytes 0 is
   THREAD_STACK_DEFAULT; others are rounded up to whole pages. No count of its own bounds them:
   memory and the address region do. */
int thread_create(uint64_t entry, uint64_t function, uint64_t argument, uint64_t stack_bytes);
int thread_join(int tid);
/* No one will join the thread: it is freed once it has ended. */
int thread_detach(int tid);
/* The calling thread ends. A process's main thread ends its process with code (sys/process.c). */
NORETURN void thread_exit(int code);
/* thread_lock held: t is to end. A thread in ring 3 on another CPU hears of it at once; one in a
   killable wait leaves it; otherwise the next killable wait it enters ends at once (sys/wait.h). */
void thread_kill_locked(struct thread *t);
/* p's main thread, ending p: every other thread of p is killed, and each one's record goes once it
   has ended (a joiner, killed too, gives up its join first). Returns once none is left. */
void thread_end_others(struct process *p);
