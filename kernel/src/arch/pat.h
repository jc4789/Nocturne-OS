#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IA32_PAT as Nocturne uses it: PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WB, PA5 WC, PA6 UC-, PA7 UC.
   vmm.c depends on index 3 (PCD|PWT) = UC for MMIO and index 5 (PWT|PAT) = WC for the framebuffer. */
#define PAT_NOCTURNE 0x0007010600070406ULL

/* Each CPU's memory types, from cpu_setup (arch/cpu.h): IA32_PAT as above, and the MTRRs as the
   first CPU found them at boot (its first call keeps them as the reference), each written only
   where this CPU's differ. False when the PAT does not read back as written. */
bool pat_cpu_setup(void);
