/* The system's one clock (sys/clock.c): nanoseconds since boot, read from a hardware counter each
   time it is asked, on any CPU, without a lock and without a tick. Every time in the kernel (a
   deadline, a sample, an interval, an event's moment) is on this axis. A device's own time (a
   sound card's frames, a display's vblank, a driver's timestamp) is tied to it by a pair: the
   device's count and clock_now_ns at it.

   The counter is the hypervisor's reference counter (vm/vm.h), else the TSC (every CPU's
   agreeing, checked as they start). Its rate comes from a
   reference of known frequency (the hypervisor's word, CPUID, the ACPI PM timer, the PIT, the
   first there is), measured at boot and again over a longer span later; the clock runs on through
   a new rate without a step. It never goes back on one CPU, nor across CPUs by more than a cache
   line takes to cross between them, and stops while the machine sleeps (S3). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NS_PER_US ((uint64_t)1000)
#define NS_PER_MS ((uint64_t)1000000)
#define NS_PER_S ((uint64_t)1000000000)
#define CLOCK_NEVER UINT64_MAX /* a deadline that never comes */

void clock_init(void); /* CPU 0 at boot, after the ACPI tables, before any other CPU starts */
/* The CPUs starting (arch/smp.c) compare their TSCs with each other and with the CPU starting
   them, all at once: it calls clock_cpus_compare with the count of CPUs comparing (itself among
   them) once every one has called clock_cpu_compare, after its LAPIC is up. Each returns with
   this CPU's readings set. */
void clock_cpu_compare(void);
void clock_cpus_compare(unsigned cpus);
uint64_t clock_now_ns(void);
uint64_t clock_tsc_hz(void); /* the TSC's rate as the clock has it, 0 where it is not known */
/* The deadline ns from now (CLOCK_NEVER past the end). */
static inline uint64_t clock_after(uint64_t ns) {
    uint64_t now = clock_now_ns();
    return ns >= CLOCK_NEVER - now ? CLOCK_NEVER : now + ns;
}
/* Any context, interrupts on or off: ns pass on the clock with the CPU spinning, where a wait may
   not sleep (a device's register sequence in a probe or a power change, uACPI's stall). */
static inline void clock_spin(uint64_t ns) {
    for (uint64_t until = clock_after(ns); clock_now_ns() < until;) __asm__ volatile("pause");
}
/* The TSC reading at which the clock reads ns, when the counter is the TSC itself (the LAPIC's
   TSC-deadline mode arms on it); false otherwise. */
bool clock_tsc_at(uint64_t ns, uint64_t *tsc);
/* The hypervisor's reference time (ns, its own epoch) when the clock reads ns; false when the
   clock is not the hypervisor's. */
bool clock_hypervisor_at(uint64_t ns, uint64_t *reference_ns);
/* S3: before sleeping, with interrupts off; after waking, before anything reads the clock (the
   TSC may have started again from 0). The clock goes on from where it stopped; the wall clock
   takes the time asleep from the RTC. clock_resume returns its seconds. */
void clock_suspend(void);
int64_t clock_resume(void);

/* The wall clock: Unix time (since 1970) as the RTC gave it at boot, moved by clock_set_wall, plus
   the time since; in ns, or in whole seconds (rounded down). */
int64_t clock_wall_ns(void);
int64_t clock_wall_seconds(void);
void clock_set_wall(int64_t unix_ns);
int64_t rtc_read_unix(void);
struct tm_parts {
    int year, mon, mday, hour, min, sec, wday;
};
void time_to_parts(int64_t t, struct tm_parts *p);
int64_t time_from_parts(const struct tm_parts *p);
