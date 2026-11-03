#pragma once
#include <stdint.h>

/* The clocks' data: the kernel writes it (kernel/src/sys/clock.c) and the code of the entry image
   reads it (kernel/entry/clock.c), which has it mapped read-only right after itself; no program
   sees this form (struct n_clock, then offset[cpus]). With t
   the TSC that RDTSCP reads (its TSC_AUX is the CPU's index, §0) less offset[that index]:
     monotonic ns = base_ns + ((t - base_tsc) * mult >> shift)   (base_ns when t <= base_tsc)
     boot ns      = monotonic ns + asleep_ns
     wall ns      = wall_ns + (monotonic ns - wall_from) + the slew's part: wall_slew's sign times
                    min(|wall_slew|, (min(monotonic ns, wall_until) - wall_from) * wall_slew_mult >> 32)
   (each difference 0 when negative). The monotonic clock is ns since the TSC began, standing still
   while the machine sleeps; the boot clock goes on through sleep; the wall clock is Unix time. A
   reader loads seq (acquire), waits while it is odd (the kernel is writing), reads the TSC with
   RDTSCP, the fields and its CPU's offset, and seq again: equal, the fields belong together; else
   it reads again. */
struct n_clock {
    uint32_t seq;
    uint32_t cpus;          /* offset[]'s entries: the CPUs the machine has */
    uint64_t base_tsc, base_ns;
    uint64_t mult;          /* ns per TSC count, times 2^shift */
    uint32_t shift;
    uint32_t rate_bound;    /* how far mult may be from the true rate, in 2^-32 parts (UINT32_MAX:
                               as the CPU states it, not measured) */
    uint64_t tsc_hz;        /* the rate mult stands for */
    uint64_t asleep_ns;
    int64_t wall_ns;
    uint64_t wall_from, wall_until;
    int64_t wall_slew;
    uint64_t wall_slew_mult;
    int64_t offset[];       /* by CPU index: what its TSC reads more than the clock's */
};
