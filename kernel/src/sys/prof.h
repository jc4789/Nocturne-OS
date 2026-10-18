#pragma once
#include "kernel.h"

struct regs;
void prof_init(void);
bool prof_enabled(void);
void prof_tick(const struct regs *r, bool idle);
void prof_ap_retire(uint64_t vector, uint64_t number);
