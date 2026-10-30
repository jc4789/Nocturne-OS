#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IA32_PAT as Nocturne uses it: PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WB, PA5 WC, PA6 UC-, PA7 UC.
   vmm.c depends on index 3 (PCD|PWT) = UC for MMIO and index 5 (PWT|PAT) = WC for the framebuffer. */
#define PAT_NOCTURNE 0x0007010600070406ULL

/* Program IA32_PAT on the calling CPU (cache and TLB flushed per the SDM/APM sequence). Run on CPU 0
   before the kernel page tables are built and on every other CPU before it uses them. Returns the MSR
   value read back. */
uint64_t pat_init_cpu(void);
/* MTRRs across a sleep: CPU 0 saves its set before it sleeps; mtrr_sync() on any CPU afterwards
   (CPU 0 when it wakes, each other CPU the kernel starts) writes that set back into that CPU's own
   MTRRs if they differ. */
void mtrr_save(void);
void mtrr_sync(void);
