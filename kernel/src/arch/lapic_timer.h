#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The wake device of sys/alarm.c: this CPU's LAPIC timer, interrupting it once on vector at the
   moment lapic_wake_at names (the monotonic clock's ns; CLOCK_NEVER: not at all) or before it,
   by no more than its rate's bound, never after it but for the interrupt's own latency (the alarm
   code sets it again when it comes early). lapic_wake_init on each CPU as it starts, CPU 0 first,
   and on CPU 0 after a sleep; both with this CPU's interrupts off. */
bool lapic_wake_init(int vector);
void lapic_wake_at(uint64_t ns);
