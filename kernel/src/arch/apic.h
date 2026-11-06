#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The local APIC (arch/apic.c), in one mode on every CPU: x2APIC mode, its registers the MSRs
   800h + offset / 16 (SDM Vol. 3A 13.12, APM Vol. 2 16.11), or xAPIC mode, its registers
   memory-mapped at offset in a 4-KiB page (13.4.1, APM 16.3.2). lapic_read and lapic_write take
   the xAPIC offset in either mode, with this CPU's interrupts off: the mode changes once at most,
   from xAPIC to x2APIC mode, on every CPU together while each has its interrupts off
   (apic_x2_follow). The timer's registers are arch/lapic_timer.c's, a maker's own entries its
   implementation's (arch/cpu_maker.h). */

#define LAPIC_VERSION 0x30
#define LAPIC_LVT_CMCI 0x2F0
#define LAPIC_LVT_TIMER 0x320
#define LVT_MASKED (1u << 16)
#define LVT_UNUSED (LVT_MASKED | 0xFE) /* masked, with a vector a Fixed entry may hold (apic_init_cpu) */
void lapic_write(uint32_t reg, uint32_t v);
uint32_t lapic_read(uint32_t reg);

/* CPU 0 once, after the ACPI tables and before topology_init: the APICs' mode. x2APIC mode where
   the firmware handed this CPU over in it (no way leads back to xAPIC mode but through the disabled
   APIC, SDM 13.12.5.1). Else, where the CPU has x2APIC mode and the DMAR does not ask the OS to
   keep out of it (VT-d 8.1, X2APIC_OPT_OUT): x2APIC mode at once where the maker lets the kernel
   enter it before interrupts are remapped (cpu_maker.x2apic_unremapped), else xAPIC mode until
   interrupts are remapped with 32-bit destinations (apic_x2_waits). Else xAPIC mode. */
void apic_choose_mode(void);
bool apic_x2(void); /* the CPUs run their APICs in x2APIC mode */
/* The CPUs are in xAPIC mode and move to x2APIC mode once the IOMMU remaps every interrupt with
   32-bit destinations (SDM 13.12.7: Extended Interrupt Mode before x2APIC mode). */
bool apic_x2_waits(void);
/* The IOMMU at boot (dev/iommu.c), its units on and no IOAPIC pin or driver's message through them
   yet, interrupts off: refused NULL when the units remap every interrupt with 32-bit destinations,
   and every online CPU moves to x2APIC mode then, all of them called in together, each with its
   interrupts off, so that none sends or takes an interrupt while their modes differ; else the CPUs
   stay in xAPIC mode, the reason logged. Nothing where none waits (apic_x2_waits). */
void apic_x2_follow(const char *refused);
/* A processor with this APIC ID may be one the firmware handed over: in xAPIC mode only those below
   FFh, the broadcast (SDM 13.12.8.1: the firmware leaves the others off), which stay the only ones
   when the CPUs move to x2APIC mode later. */
bool apic_handed_over(uint32_t apic_id);

/* CPU 0 once (irq_init), after the MADT, the heap and the kernel's page tables: the NMI inputs the
   MADT names, the error vector's handler, the registers' page in xAPIC mode, and where the CPUs
   wait for x2APIC mode the vector that calls them in to move (VEC_APIC_MODE). */
void apic_init(void);
/* Each CPU as it starts, and again after S3 (which resets it): its APIC in the CPUs' mode, the task
   priority open, every LVT entry masked (the maker's own too) but the error one and the NMI inputs
   the MADT names, the APIC enabled with the spurious vector. False when this CPU's APIC cannot be
   put in that mode. */
bool apic_init_cpu(void);
uint32_t apic_current_id(void);
void apic_eoi(void); /* the end of the interrupt this CPU is handling (arch/irq.c) */
/* vector is pending at this CPU's APIC: its bit in the IRR (SDM 13.8.4, APM 16.6.3). Interrupts off. */
bool apic_irr(uint8_t vector);
/* The interrupt this CPU took on vector came as level-triggered: its bit in the TMR, which the
   APIC sets on accepting a level interrupt and clears on an edge one, and by which its EOI goes on
   to the IOAPICs (SDM 13.8.4). Before the EOI, interrupts off. */
bool apic_tmr(uint8_t vector);

/* IPIs: fixed delivery of vector, edge, to CPUs by topology's index (arch/topology.h). set: a CPU
   set (arch/cpuset.h), in x2APIC mode in as few ICR writes as the logical clusters allow (up to 16
   CPUs each, SDM 13.12.10.2), in xAPIC mode one each by its APIC ID; others: every online CPU but
   this one. Any CPU, any context, no wait for the IPI to be taken: interrupts off for each, in
   x2APIC mode one WRMSR (the ICR has no delivery status, 13.12.9), in xAPIC mode the two halves of
   the ICR after the previous IPI left (13.6.1). */
void apic_send_ipi(unsigned cpu, uint8_t vector);
void apic_send_ipi_set(const uint64_t *cpus, uint8_t vector);
void apic_send_ipi_others(uint8_t vector);
void apic_send_nmi(unsigned cpu); /* reaches a CPU whose interrupts are off */
/* INIT and STARTUP, sent and not waited for (SDM 11.4.4.1, Table 11-1): the caller waits
   LAPIC_INIT_WAIT_NS after its INITs before any STARTUP, and sends STARTUP twice, waiting
   LAPIC_STARTUP_WAIT_NS after each round, the second ignored by a CPU already running. One wait
   serves every CPU sent to. */
#define LAPIC_INIT_WAIT_NS 10000000ULL
#define LAPIC_STARTUP_WAIT_NS 200000ULL
void apic_send_init(unsigned cpu); /* INIT: the CPU waits for a STARTUP, running nothing */
void apic_send_startup(unsigned cpu, uint64_t page); /* after an INIT: into real mode at page, below 1 MiB */
