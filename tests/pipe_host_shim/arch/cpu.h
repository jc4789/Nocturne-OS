#pragma once
#include <stdint.h>
uint64_t irq_save(void);
void irq_restore(uint64_t);
