#pragma once
/* Per-CPU variables. A variable defined PERCPU lives in the .percpu section, which is only their
   template: every CPU has a copy of the whole section, in memory on its own node (percpu_init),
   and its GS base is that copy's distance from the template, so that the GS-relative form of a
   variable's own address (clang's __seg_gs, RIP-relative) is this CPU's copy, one instruction.
   Its TSC_AUX is its index (RDPID), from which the entries that may come in under the app's GS
   base (NMI, #DB, #MC, #DF; arch/isr.asm) find the distance in percpu_offsets.

   Before percpu_init the first CPU's GS base is 0: the template is its copy. Only its GDT and TSS
   are written there meanwhile, and every CPU writes its own afresh (gdt_init_cpu). */
#include <stdint.h>

#define PERCPU __attribute__((section(".percpu")))
/* This CPU's copy of v, an lvalue, one instruction. Where the code is preemptible (arch/preempt.h)
   the next access may be another CPU's copy: a read, change and write in steps holds preemption off
   meanwhile. Atomics take this_cpu_ptr's plain pointer. */
#define this_cpu(v) (*(__typeof__(v) __seg_gs *)(uintptr_t)&(v))
/* A plain pointer to this CPU's copy of v, which stays this CPU's only while preemption is off. */
#define this_cpu_ptr(v) ((__typeof__(v) *)((uintptr_t)&(v) + this_cpu(percpu_offset)))
/* CPU cpu's copy of v (topology's index), an lvalue. */
#define per_cpu(v, cpu) (*(__typeof__(v) *)((uintptr_t)&(v) + percpu_offsets[cpu]))

extern PERCPU uintptr_t percpu_offset; /* this CPU's copy's distance from the template */
extern PERCPU unsigned cpu_index;      /* this CPU's index in topology (arch/topology.h) */
extern uintptr_t *percpu_offsets;      /* [topology.cpus] */

/* The first CPU, after topology_init and bootmem_by_node and before anything keeps state per CPU:
   a copy for every CPU on its node, and the first CPU onto its own. */
void percpu_init(void);
/* This CPU's GS base, its copy's (cpu: its index), once its GDT's segments are loaded; and its
   TSC_AUX, the index. */
void percpu_load(unsigned cpu);
