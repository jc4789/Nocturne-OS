#pragma once
#include "kernel.h"

/* Bare-metal CPU clock: request the highest performance state on every CPU
   and measure the effective clock with APERF/MPERF. */
void pstate_init_bsp(void);     /* after the TSC is calibrated */
void pstate_init_ap(void);      /* on each AP before it runs work */
bool pstate_sampling(void);
void pstate_sample(void);       /* the calling CPU, interrupts off, often: closes a window of a second */
bool pstate_cpu(unsigned cpu, unsigned *mhz, unsigned *awake_permille);
