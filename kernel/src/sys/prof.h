#pragma once
#include "kernel.h"

/* CPU 0 at boot, before the other CPUs start: whether the command line asks for samples. */
void prof_init(void);
/* Each CPU as it starts, after its alarms (sys/alarm.h): its samples begin. */
void prof_cpu_start(void);
