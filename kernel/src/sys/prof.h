#pragma once
#include "kernel.h"

struct regs;
void prof_init(void);
bool prof_enabled(void);
void prof_tick(const struct regs *r, bool idle);
struct task;
/* An AP syscall's round trip through the BSP, in three legs: retired on the
   AP until the BSP takes it, the BSP's work, and the handoff until an AP runs
   it again. */
void prof_ap_retire(struct task *t, uint64_t vector, uint64_t number);
void prof_retire_taken(struct task *t);
void prof_retire_handoff(struct task *t);
void prof_retire_resumed(struct task *t);
