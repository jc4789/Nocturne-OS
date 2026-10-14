#pragma once
#include "kernel.h"
/* Only scheduler execution is replaced; production VFS registrations and
   provider functions are compiled unchanged into the host check. */
struct task { bool killed; };
struct wait_queue { struct task *head; unsigned wakes; };
extern struct task *current_task;
void wq_wait(struct wait_queue *q);
void wq_wake_all(struct wait_queue *q);
