#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The local APIC (arch/apic.c), in x2APIC mode only (SDM Vol. 3A 13.12, APM Vol. 2 16.11): its
   registers are the MSRs 800h + offset / 16. The timer's registers are arch/lapic_timer.c's. */

#define LAPIC_LVT_TIMER 0x320
#define LVT_MASKED (1u << 16)
#define LVT_UNUSED (LVT_MASKED | 0xFE) /* masked, with a vector a Fixed entry may hold (apic_init_cpu) */
void lapic_write(uint32_t reg, uint32_t v);
uint32_t lapic_read(uint32_t reg);

/* CPU 0 once (irq_init), after the MADT and the heap: the NMI inputs the MADT names, the error
   vector's handler. */
void apic_init(void);
/* Each CPU as it starts, and again after S3 (which resets it): its APIC in x2APIC mode, the task
   priority open, every LVT entry masked but the error one and the NMI inputs the MADT names, the
   APIC enabled with the spurious vector. False when this CPU's APIC cannot be put in x2APIC mode. */
bool apic_init_cpu(void);
uint32_t apic_current_id(void);
void apic_eoi(void); /* the end of the interrupt this CPU is handling (arch/irq.c) */

/* IPIs: fixed delivery of vector, edge, to CPUs by topology's index (arch/topology.h). set: a CPU
   set (arch/cpuset.h), in as few ICR writes as the x2APIC logical clusters allow (up to 16 CPUs
   each, SDM 13.12.10.2); others: every online CPU but this one. Any CPU, any context: one WRMSR
   each, no wait (x2APIC's ICR has no delivery status, 13.12.9). */
void apic_send_ipi(unsigned cpu, uint8_t vector);
void apic_send_ipi_set(const uint64_t *cpus, uint8_t vector);
void apic_send_ipi_others(uint8_t vector);
void apic_send_nmi(unsigned cpu); /* reaches a CPU whose interrupts are off */
/* INIT and STARTUP, sent and not waited for (SDM 9.4.4.1, Table 9-1): the caller waits
   LAPIC_INIT_WAIT_NS after its INITs before any STARTUP, and sends STARTUP twice, waiting
   LAPIC_STARTUP_WAIT_NS after each round, the second ignored by a CPU already running. One wait
   serves every CPU sent to. */
#define LAPIC_INIT_WAIT_NS 10000000ULL
#define LAPIC_STARTUP_WAIT_NS 200000ULL
void apic_send_init(unsigned cpu); /* INIT: the CPU waits for a STARTUP, running nothing */
void apic_send_startup(unsigned cpu, uint64_t page); /* after an INIT: into real mode at page, below 1 MiB */
