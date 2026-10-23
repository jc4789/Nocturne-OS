/* kernel/src/sys/sched.h on the build host, where one thread does everything: a lock is never
   contended and nothing waits. */
#pragma once
#include "kernel.h"

struct wait_queue {
    int unused;
};
struct mutex {
    unsigned depth;
};
static inline void mutex_lock(struct mutex *m) { m->depth++; }
static inline void mutex_unlock(struct mutex *m) { m->depth--; }
static inline void wq_wake_all(struct wait_queue *wq) { (void)wq; }
/* nothing else runs to end a wait */
static inline void wq_wait(struct wait_queue *wq) { (void)wq; panic("store: a wait on the host"); }
