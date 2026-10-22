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
   upper half, with rsp = stack and rdi = arg, interrupts off, no IDT, the page's own GDT.
   apic_id: the AP a STARTUP is for, sent after its INIT and this call (arch/smp.c), or
   WAKE_ANY_CPU for the BSP the firmware wakes, an ID no APIC has (SDM 11.12.1.3: x2APIC ID
   FFFFFFFFh is reserved). In xAPIC mode a STARTUP can reach other CPUs too: INIT turns off the
   8-bit ID of an AMD APIC's extended register space (APM 16.3.2, Table 16-2), after which it
   compares a destination's low four bits only (16.3.6). Each reads its APIC
   ID register (SDM 9.4.4.2) before it touches the stack; one that is not apic_id stops in the
   page, halted with interrupts off, until an INIT. An AP comes in the APIC mode the firmware left
   it in, the kernel's or not (INIT keeps it, SDM 11.12.5): in x2APIC mode it reads the x2APIC ID
   MSR; in xAPIC mode, with the kernel's x2APIC mode, it takes that mode first, which keeps its
   x2APIC ID; with the kernel's xAPIC mode it reads the register at the kernel's xAPIC page
   (struct lapic_ident), and stops where IA32_APIC_BASE puts its own APIC elsewhere. */
#define WAKE_ANY_CPU UINT32_MAX
void wake_prepare(uint64_t entry, uint64_t stack, uint64_t arg, uint32_t apic_id);

/* BSP, interrupts off: save this CPU (control registers, MSRs, GDT/IDT/TSS, FPU, MTRRs) and call
   enter(), which puts the machine to sleep. Returns true after the firmware woke it and this CPU
   is back as it was; false when enter() returned instead (the machine did not sleep). The interrupt
   controllers, the timer and the devices are the caller's. */
bool wake_sleep(void (*enter)(void));

/* Hibernation restore (dev/hibernate.c). The spare page follows the wake pages below 1 MiB, never
   used otherwise (0 when low memory has no room for it). wake_hibernate_jump runs from it on pml4,
   a page table mapping all RAM one to one (its kernel half is filled in here): it copies every
   pair of the list (arch/wake.asm) and enters the image through its wake page, where wake_sleep
   returns true. Interrupts off, every other CPU in INIT, every device stopped. */
uint64_t wake_spare_page(void);
__attribute__((noreturn)) void wake_hibernate_jump(uint64_t pml4, uint64_t list, uint64_t image_wake_page);
