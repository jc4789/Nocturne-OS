#pragma once
/* What a CPU does with no task to run, and how fast it runs when it has one. */
#include <stdbool.h>
#include <stdint.h>

/* The idle task's wait (sys/sched.c), interrupts off on the way in and on the way back: until an
   interrupt, or until another CPU tells it through cpu_idle_wake. With MWAIT it waits on a word of
   its own (C1: AMD's MWAIT goes no deeper, Family 19h PPR CPUID 5); without it, in HLT. The caller
   looks at its queue again after each return, whatever ended the wait. */
void cpu_idle(void);
/* Another CPU, whose queue just got a task: true when it was waiting on its word and has been
   told by this write (no IPI is needed), false when it has to be interrupted. */
bool cpu_idle_wake(unsigned cpu);

/* The performance each CPU asks for, through CPPC where the CPU has it (ACPI 6.5 8.4.6.1): the
   range from its lowest nonlinear performance to its highest, the hardware choosing within it by
   an energy-performance preference from 0 (performance) to 255 (energy), 8.4.6.1.7. The default
   leans to performance; the setting is the system's, which may change it at any time. */
#define CPU_PERF_PREFERENCE_DEFAULT 0x40 /* a balance leaning to performance */
void cpu_perf_setup(void);                      /* each CPU, from cpu_setup (arch/cpu.h) */
void cpu_perf_set_policy(uint8_t preference);   /* every online CPU takes it at once */
/* The effective clock (APERF/MPERF, AMD APM vol. 2 17.3; Intel SDM vol. 3B 17.2): each CPU,
   sampled often with interrupts off (the profiler, sys/prof.c), closes a window of a second; the
   last window's MHz and the share of it the CPU was awake, in thousandths. */
bool cpu_perf_sampling(void);
void cpu_perf_sample(void);
bool cpu_perf_of(unsigned cpu, unsigned *mhz, unsigned *awake_permille);
