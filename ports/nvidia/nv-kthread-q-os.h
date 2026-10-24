/* What nv-kthread-q.h (NVIDIA, kernel-open/common/inc) leaves to the OS: the queue and item
 * structures. On Linux the queue is a kernel thread; in nvrm it is a C11 thread (uvm_os.c
 * implements the nv_kthread_q_* functions). Only UVM uses these queues. */
#ifndef NVRM_NV_KTHREAD_Q_OS_H
#define NVRM_NV_KTHREAD_Q_OS_H

#include "uvm_os.h"

struct nv_kthread_q
{
    struct list_head q_list_head;   /* items scheduled and not yet run, in order */
    spinlock_t q_lock;
    struct semaphore q_sem;         /* one count per scheduled item, plus one to stop */
    atomic_t main_loop_should_exit;
    struct task_struct *q_kthread;  /* NULL when the queue is not running */
    bool is_unload_flush_ongoing;
};

struct nv_kthread_q_item
{
    struct list_head q_list_node;   /* empty while not scheduled */
    nv_q_func_t function_to_run;
    void *function_args;
};

#define NV_KTHREAD_NO_NODE NUMA_NO_NODE

#endif
