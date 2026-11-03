/* The entry image (common/abi.h): every process's one way into the kernel. */
#pragma once
#include "kernel.h"

struct process;

/* Makes the images' frames: after clock_init, before the first program starts. */
void entry_init(void);
/* Maps the image into a process being started, at an address of its own (process->entry_base). With
   all_calls every slot enters the kernel; otherwise only the public ones (N_PUBLIC_CALLS). */
int entry_map(struct process *p, bool all_calls);
/* Whether a SYSCALL that returns to rip, with this number, came from the number's own slot. */
bool entry_came_from(const struct process *p, uint64_t rip, uint64_t number);
