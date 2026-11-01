#pragma once
/* How far the code running on this CPU is from being preemptible: one count per CPU, which only
   that CPU changes, in three parts:
     bits 0-15   preemption held off: preempt_disable, and every spinlock this CPU holds (spin_take)
     bits 16-23  the interrupt handlers this CPU is inside (arch/isr.asm, isr_interrupt)
     bits 24-27  the NMIs and machine checks this CPU is inside, which come with interrupts off
   An entry that changes it gives back what it took before it leaves, so a plain read-modify-write
   is never lost to one that interrupts it on the same CPU.

   The kernel is preempted where the count is 0 and interrupts are on (preemptible), once the
   scheduler says a switch is due (sched_resched, sys/sched.c): at preempt_enable when the count
   comes back to 0, at irq_restore when interrupts come back on (arch/cpu.h), on the way out of an
   interrupt into the kernel (arch/isr.asm) and on the way back to ring 3. Code that keeps a
   pointer to this CPU's copy of a variable (this_cpu_ptr), or reads and changes one in steps, holds
   preemption off meanwhile: a spinlock, interrupts off, or preempt_disable. */
#include <stdbool.h>
#include <stdint.h>
#include "arch/cpu.h"
#include "arch/percpu.h"

extern PERCPU uint32_t preempt_count; /* arch/smp.c */

#define PREEMPT_ONE      1u
#define PREEMPT_MASK     0xFFFFu
#define PREEMPT_IRQ_ONE  (1u << 16)
#define PREEMPT_IRQ_MASK (0xFFu << 16)
#define PREEMPT_NMI_ONE  (1u << 24)
#define PREEMPT_NMI_MASK (0xFu << 24)

static inline void preempt_disable(void) {
    this_cpu(preempt_count) += PREEMPT_ONE;
    __asm__ volatile("" ::: "memory");
}
static inline void preempt_enable(void) {
    __asm__ volatile("" ::: "memory");
    if (!(this_cpu(preempt_count) -= PREEMPT_ONE) && this_cpu(sched_resched)) preempt_schedule();
}
/* Inside an interrupt's handler, an NMI's or a machine check's. */
static inline bool in_interrupt(void) { return (this_cpu(preempt_count) & (PREEMPT_IRQ_MASK | PREEMPT_NMI_MASK)) != 0; }
/* Inside an NMI's or a machine check's handler, which may have come in anywhere. */
static inline bool in_nmi(void) { return (this_cpu(preempt_count) & PREEMPT_NMI_MASK) != 0; }
static inline bool preemptible(void) { return !this_cpu(preempt_count) && (read_rflags() & 0x200); }
