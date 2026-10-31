#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/percpu.h"
#include "arch/topology.h"

/* The CPUs are topology's (arch/topology.h): CPU i is topology.cpu[i], the first CPU 0. */
static inline unsigned cpu_current_index(void) { return this_cpu(cpu_index); }

struct n_cpuinfo;
struct regs;

/* Every CPU is the same: each runs the scheduler's tasks (sys/sched.c), takes its own traps,
   faults and system calls, serves interrupts sent to it and fires its own alarms (sys/alarm.c).
   CPU 0 is the first only in that it starts the others and takes the devices' interrupts.

   smp_init: CPU 0, once its scheduler and alarms run: starts every other CPU topology found at
   once through the wake page (arch/wake.h) and returns once each has come online or been left
   out. */
void smp_init(void);
unsigned cpu_online_count(void);
bool cpu_online_at(unsigned index);
/* The CPUs running the scheduler (arch/cpuset.h, topology.cpus bits): a CPU joins it as it starts
   and leaves it for a sleep. */
const uint64_t *cpu_online_set(void);
/* Have that CPU look at its run queue: a write to the word it idles on (arch/idle.h), or an IPI.
   Never the calling CPU. */
void cpu_kick(unsigned index);
bool cpu_tlb_ipi(unsigned index); /* false when that CPU is not running */
/* Interrupt that CPU's alarms (sys/alarm.c): it fires what is due and sets its wake device again. */
void cpu_alarm(unsigned index);
void smp_get_info(struct n_cpuinfo *info);
/* panic (lib/printf.c): true on the CPU that reports, every other CPU stopped first; false on one
   that is to halt (another CPU reports, or this one faulted while reporting too often). */
bool smp_panic_enter(void);
/* An NMI (arch/idt.c), on its IST stack: a CPU stopped for another's panic never returns from it;
   any other is counted, told where it came in, and returned from. */
void smp_nmi(struct regs *r);

/* Sleep: smp_suspend (CPU 0, interrupts on, every app stopped) takes every other CPU's tasks
   back to CPU 0 and halts all of those CPUs at once with interrupts off, false when one did not
   stop in time (or there is no wake page or memory to start them again with); smp_resume starts
   them all again at once through the wake page, after S3 or after a sleep that failed.
   cpu_park_check is the idle loop's side, interrupts off. */
bool smp_suspend(void);
void smp_resume(void);
void cpu_park_check(void);
void smp_hold_in_init(void); /* after smp_suspend, for a hibernation restore: every AP to INIT */
