#pragma once
#include "kernel.h"
uint64_t irq_save(void);
void irq_restore(uint64_t flags);
