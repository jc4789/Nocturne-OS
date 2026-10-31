#pragma once
/* CPUs that start in real mode (arch/wake.asm): the APs the kernel starts with INIT-SIPI, at boot
   and after a sleep, and the BSP woken from S3 by the firmware. All enter one page below 1 MiB,
   the wake page, which wake_prepare_any() or wake_prepare_cpus() sets up for the next CPUs to come
   in. */
#include <stdbool.h>
#include <stdint.h>

void wake_init(void);      /* after vmm_init: find the wake page in low memory */
bool wake_available(void); /* false when no usable page below 1 MiB was found */
uint64_t wake_page(void);  /* its physical address: the FACS waking vector, the SIPI page */
/* A CPU through the wake page enters x2APIC mode, then jumps to entry in long mode on the kernel's
   page tables' upper half, with rsp = stack and rdi = arg, interrupts off, no IDT, the page's own
   GDT.

   wake_prepare_any: the BSP the firmware wakes from S3, the one CPU to come in, takes them as
   they are given.
   wake_prepare_cpus: the APs that STARTUPs are sent to after their INITs and this call
   (arch/smp.c), each its row of table (count rows, in the kernel's memory, kept until they are
   through), found by its x2APIC ID. A CPU a STARTUP reached with no row stops in the page, halted
   with interrupts off, until an INIT. */
struct wake_cpu {
    uint64_t stack, arg;
    uint32_t apic_id, pad;
};
void wake_prepare_any(uint64_t entry, uint64_t stack, uint64_t arg);
void wake_prepare_cpus(uint64_t entry, const struct wake_cpu *table, uint32_t count);

/* CPU 0, interrupts off, every other CPU parked: save what is this task's own (its FS and GS
   bases, its extended state) and call enter(), which puts the machine to sleep. Returns true after
   the firmware woke it, this CPU set up again by cpu_setup (arch/cpu.h) on the kernel's tables;
   false when enter() returned instead (the machine did not sleep). The interrupt controllers, the
   timer and the devices are the caller's. */
bool wake_sleep(void (*enter)(void));

/* Hibernation restore (dev/hibernate.c). The spare page follows the wake pages below 1 MiB, never
   used otherwise (0 when low memory has no room for it): the restore's last copy runs from it.
   wake_long_entry: where a CPU in long mode on the wake page's own tables enters page's code, with
   rbx = page (the image's wake page, which its wake_sleep prepared before the snapshot). */
uint64_t wake_spare_page(void);
uint64_t wake_long_entry(uint64_t page);
