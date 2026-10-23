/* kernel/src/arch/cpu.h on the build host: no interrupts to hold off. */
#pragma once
#include <stdint.h>

static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t f) { (void)f; }
