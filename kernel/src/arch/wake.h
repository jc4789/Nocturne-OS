#pragma once
/* CPUs that start in real mode (arch/wake.asm): the BSP woken from S3 by the firmware, and APs the
   kernel starts itself with INIT-SIPI (after S3 Limine's MP start is gone). Both enter one page
   below 1 MiB, the wake page, which wake_prepare() sets up for the next CPU to come in. */
#include <stdbool.h>
#include <stdint.h>

void wake_init(void);      /* after vmm_init: find the wake page in low memory */
bool wake_available(void); /* false when no usable page below 1 MiB was found */
uint64_t wake_page(void);  /* its physical address: the FACS waking vector, the SIPI page */
/* The next CPU through the wake page jumps to entry in long mode on the kernel's page tables'
   upper half, with rsp = stack and rdi = arg, interrupts off, no IDT, the page's own GDT. */
void wake_prepare(uint64_t entry, uint64_t stack, uint64_t arg);

/* BSP, interrupts off: save this CPU (control registers, MSRs, GDT/IDT/TSS, FPU, MTRRs) and call
   enter(), which puts the machine to sleep. Returns true after the firmware woke it and this CPU
   is back as it was; false when enter() returned instead (the machine did not sleep). The interrupt
   controllers, the timer and the devices are the caller's. */
bool wake_sleep(void (*enter)(void));
