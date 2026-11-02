#pragma once
#include "kernel.h"

/* CPU 0 at boot, before the other CPUs start: samples every CPU and prints them every ms
   milliseconds. Nothing calls it now, so the kernel takes no samples. */
void prof_init(unsigned ms);
/* Each CPU as it starts, after its alarms (sys/alarm.h): its samples begin. */
void prof_cpu_start(void);
/* The window the samples are printed in (ns), 0 while nothing is sampled: owners of their own
   counts (the compositor's frames, gui/wm.c) print theirs in the same window. Any context. */
uint64_t prof_window_ns(void);
