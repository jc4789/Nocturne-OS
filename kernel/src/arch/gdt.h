#pragma once
#include <stdint.h>

/* Each CPU's GDT and TSS (arch/gdt.c). */

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B
#define USER_CS   0x23
#define TSS_SEL   0x28

/* Each CPU's stacks for the exceptions that may come where the stack in use cannot be trusted
   (the TSS's IST, arch/gdt.c): NMI, #DB and #MC anywhere, #DF after a stack went bad. */
enum { IST_DOUBLE_FAULT = 1, IST_NMI, IST_DEBUG, IST_MACHINE_CHECK, IST_STACKS = IST_MACHINE_CHECK };

/* From cpu_setup (arch/cpu.h), and on the first CPU again from percpu_init, which moves its GDT
   and TSS from the template into its own copy. */
void gdt_init_cpu(unsigned index);
void tss_set_rsp0(uint64_t rsp0);
void tss_set_ist(const uint64_t top[IST_STACKS]);
