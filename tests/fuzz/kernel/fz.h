/* Host-side fuzzing prelude, force-included (-include) before every product
   translation unit. It pulls in the real kernel headers and then replaces the
   few privileged / per-CPU inline helpers (irq_save, %gs-relative current_task)
   with host equivalents. No product source is modified. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"

extern struct task fz_task;
extern volatile bool fz_resched;

#undef current_task
#define current_task (&fz_task)
#undef need_resched
#define need_resched fz_resched
#define task_process() (&fz_task)
#define irq_save() ((uint64_t)0)
#define irq_restore(f) ((void)(f))
