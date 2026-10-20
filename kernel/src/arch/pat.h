#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IA32_PAT as Nocturne uses it: PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WB, PA5 WC, PA6 UC-, PA7 UC.
   vmm.c depends on index 3 (PCD|PWT) = UC for MMIO and index 5 (PWT|PAT) = WC for the framebuffer. */
#define PAT_NOCTURNE 0x0007010600070406ULL

bool pat_supported(void);
/* Program IA32_PAT on the calling CPU (cache and TLB flushed per the SDM/APM sequence). Run on the BSP
   before the kernel page tables are built and on every AP before it uses them. Returns the MSR value
   read back, or 0 when the CPU has no PAT. */
uint64_t pat_init_cpu(void);
/* MTRRs across a sleep: the BSP saves its set before it sleeps; mtrr_sync() on any CPU afterwards
   (the BSP when it wakes, each AP the kernel starts) writes that set back if it differs. */
void mtrr_save(void);
void mtrr_sync(void);
