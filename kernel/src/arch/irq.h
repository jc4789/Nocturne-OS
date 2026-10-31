#pragma once
/* The interrupt layer (arch/irq.c, arch/idt.c, arch/apic.c, arch/ioapic.c): what each vector of
   each CPU delivers, and the sources behind them (message-signalled ones, the IOAPICs' global
   system interrupts, the local APIC's own).

   Vectors (x86 has 256; SDM Vol. 3A 6.2: 0-31 are the exceptions; 13.8.3: bits 7:4 are the
   priority class, and the APIC hands the core the highest pending vector first) are each CPU's
   own: every CPU has its table of what its vectors deliver, so the vectors grow with the CPUs.
   The system vectors are the same on every CPU, at the top. A device's are given per CPU from
   VEC_DEVICE_FIRST to VEC_DEVICE_LAST, its CPU chosen in the device's node (fewest vectors in
   use) among the CPUs its interrupts can name: an APIC ID above 255 only where interrupt
   remapping carries 32-bit destinations (iommu_irq_wide), the plain MSI address and the IOAPIC's
   redirection entry holding 8 bits (SDM 13.11.1; 82093AA). No driver has a fixed vector. */
#include <stdbool.h>
#include <stdint.h>
#include "arch/cpu.h"
#include "arch/percpu.h"

#define VEC_PIC_BASE      0x20 /* the 8259's 16, moved here and all masked (irq_init): none comes */
#define VEC_DEVICE_FIRST  0x30
#define VEC_DEVICE_LAST   0xEF
#define VEC_SYSTEM_FIRST  0xF0
#define VEC_LOG           0xF0 /* a CPU's own, after it wrote a record: wakes the log's readers (sys/log.c) */
#define VEC_MCE_THRESHOLD 0xFA /* AMD's machine check threshold, through an extended LVT entry */
#define VEC_TIMER         0xFB /* each CPU's wake device (arch/lapic_timer.c, sys/alarm.c) */
#define VEC_RESCHED       0xFC /* look at this CPU's run queue (sys/sched.c) */
#define VEC_TLB_SHOOTDOWN 0xFD /* mm/vmm.c */
#define VEC_APIC_ERROR    0xFE /* the local APIC's error LVT entry (arch/apic.c) */
#define VEC_SPURIOUS      0xFF /* the local APIC's spurious vector: no handler, no EOI (13.9) */

enum irq_result { IRQ_NONE, IRQ_HANDLED, IRQ_WAKE_THREAD };
struct irq_spec {
    const char *name;
    enum irq_result (*handler)(void *context);   /* hard: short, interrupts off */
    void (*thread)(void *context);               /* optional: runs in the irq's own kernel thread */
    void *context;
    int node;                                    /* the device's NUMA node, -1 when unknown */
};
struct irq;

/* The hard handler runs on the irq's CPU with interrupts off, on that CPU's interrupt stack; the
   EOI follows it (SDM 13.8.5). IRQ_WAKE_THREAD runs the thread once more, in a kernel thread of
   the irq's own bound to the irq's CPU (kthread_create_on); without a handler every interrupt
   wakes it. A level-triggered line stays masked from its hard handler until every thread it
   woke has run. Lines are shared: each handler of one says whether the interrupt was its device's.

   cpu: the CPU to deliver to (topology's index), or IRQ_CPU_ANY for the layer's choice; a CPU
   that is offline, has no vector free, or whose APIC ID the interrupt cannot name is refused.
   NULL when no CPU it may name has a vector free, or without memory. A spec has a handler, a
   thread, or both.
   irq_request_msi: a vector whose message the caller writes (irq_msi_message: the plain form
   for a destination of 8 bits, irq_target for an interrupt remapping entry).
   irq_request_msi_block: count (a power of two up to 32) vectors in one aligned run on one CPU,
   for MSI's multiple messages (PCI Local Bus 3.0, 6.8.1.3: message n is data + n), specs[i] for
   message i: false when it cannot.
   irq_request_gsi: the IOAPIC pin's, its trigger and polarity as the MADT and ACPI describe them
   (arch/ioapic.h); a level pin is shared, an edge pin is not.
   irq_request_system: a system vector (VEC_SYSTEM_FIRST up to VEC_APIC_ERROR) on every CPU, for
   the local APIC's own sources and the IPIs; a handler, no thread. Any time after the heap.
   Any CPU; the MSI and GSI requests and irq_free sleep (a thread, a mutex), never from an
   interrupt. */
#define IRQ_CPU_ANY UINT32_MAX
struct irq *irq_request_msi(const struct irq_spec *spec, unsigned cpu);
bool irq_request_msi_block(const struct irq_spec *specs, unsigned count, unsigned cpu, struct irq **irqs);
void irq_msi_message(const struct irq *irq, uint64_t *address, uint32_t *data);
struct irq *irq_request_gsi(const struct irq_spec *spec, unsigned gsi);
struct irq *irq_request_system(const struct irq_spec *spec, unsigned vector);
/* Returns once no handler or thread of it runs on any CPU, nor will (its source the caller turned
   off first: a message that still comes is a stray). Never from its own handler or thread. */
void irq_free(struct irq *irq);
/* To that CPU: a GSI's entry is written again; a message by its writer (irq_msi_writer), false
   for one without, and for a block's (its vectors stay together) and a system vector. */
bool irq_set_cpu(struct irq *irq, unsigned cpu);
/* Where it is delivered: the CPU (topology's index), its APIC ID, the vector; a system vector's
   CPU is IRQ_CPU_ANY (every one). */
struct irq_target {
    unsigned cpu;
    uint32_t apic_id;
    uint8_t vector;
};
struct irq_target irq_target(const struct irq *irq);
/* Whoever writes the message of a message-signalled irq (dev/pci.c): irq_set_cpu calls write to
   put the new one in place (irq_msi_message, irq_target say the new target by then); false
   when it could not, the old one then kept. */
void irq_msi_writer(struct irq *irq, bool (*write)(struct irq *irq, void *writer), void *writer);
/* The frame the interrupt handled on this CPU came in on (the profiler's sample), NULL outside
   a handler. */
struct regs *irq_regs(void);

/* CPU 0 once, after the MADT and the heap, before any MSI or GSI request: the 8259 moved and
   masked, the IOAPICs found and masked, the local APICs' NMI inputs read and this CPU's APIC set
   (apic_init_cpu; every other CPU sets its own as it starts). suspend and resume: CPU 0 around
   S3, interrupts off: the IOAPICs' entries masked, then (the chips reset) the 8259 masked again
   and every entry made again, after the IOMMU (iommu_resume) and this CPU's APIC. */
void irq_init(void);
void irq_suspend(void);
void irq_resume(void);

/* The ways into the kernel (arch/idt.c, arch/isr.asm): idt_init builds the IDT on the first CPU
   and loads its entries there, entry_load loads them on each other CPU as it starts (the IDT and
   SYSCALL's MSRs). entry_stacks_init gives this CPU its interrupt and IST stacks, once the page
   tables can map them (vmm_init), before its interrupts are on: false without the memory. */
void idt_init(void);
void entry_load(void);
bool entry_stacks_init(void);
/* arch/isr.asm's interrupt entry (vectors 32 and up), on this CPU's interrupt stack. */
void isr_interrupt(struct regs *r);
