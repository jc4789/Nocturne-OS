/* The system's clocks (sys/clock.c), read from the TSC each time they are asked, on any CPU,
   without a lock and without a tick.

   - The monotonic clock: ns since the TSC began (the CPU's reset), standing still while the
     machine sleeps. Every time in the kernel (a deadline, a sample, an interval, an event's
     moment) is on it. A device's own time (a sound card's frames, a display's vblank) is tied to
     it by a pair: the device's count and clock_now_ns at it.
   - The boot clock: the monotonic clock plus the time spent asleep (asleep_ns below).
   - The wall clock: Unix time, from a wall-time reference (the RTC at a second's edge) and on with
     the boot clock; clock_set_wall steps it, clock_adjust_wall slews it.

   The TSC is the only counter. Its rate is stated from the start: on the hardware the P0
   P-state's (arch/cpu.h: the TSC counts at its frequency), under a hypervisor the one it
   registers (clock_register_tsc_hz). The P0 rate is nominal (spread spectrum lowers it, the
   crystal is off by its tolerance): on the hardware a rate reference (the HPET, dev/refclock.h)
   measures it again and again over windows that double, each new rate taken from the clock's
   present reading on (its slope changes, the clock never steps or goes back), each with the bound
   of its error that the measurement proves. The CPUs' TSCs are compared as they start
   (clock_cpus_compare); what one is seen behind by is added to its readings. A reading never goes
   back on one CPU, nor across CPUs by more than the comparison can see (a cache line's crossing).

   The references. What keeps better time than the stated rate tells the clock through one entry
   for each kind, whoever tells it: a rate reference gives samples of its count
   (clock_rate_sample; the HPET now), a wall-time reference proves the time
   (clock_wall_proof; the RTC now, which the clock reads itself). A network time service would be
   one of each.

   All of it lives in the clock's data (struct clock_data) in pages of their own: the kernel
   writes them, under a sequence count; readers retry while it is odd. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PER_US ((uint64_t)1000)
#define NS_PER_MS ((uint64_t)1000000)
#define NS_PER_S ((uint64_t)1000000000)
#define CLOCK_NEVER UINT64_MAX /* a deadline that never comes */
#define CLOCK_RATE_UNMEASURED UINT32_MAX /* a rate's bound before any reference measured it */

/* The clock's data, as its pages hold it. With t this CPU's TSC (read by RDTSCP, whose TSC_AUX
   is the CPU's index) less offset[that index]:
     monotonic ns = base_ns + ((t - base_tsc) * mult >> shift)   (base_ns when t <= base_tsc)
     boot ns      = monotonic ns + asleep_ns
     wall ns      = wall_ns + (monotonic ns - wall_from) + the slew's part: wall_slew's sign times
                    min(|wall_slew|, (min(monotonic ns, wall_until) - wall_from) * wall_slew_mult >> 32)
   A reader loads seq (acquire), reads the TSC, the fields, and seq again: equal and even, the
   fields belong together; else it reads again. */
struct clock_data {
    uint32_t seq;
    uint32_t cpus;          /* offset[]'s entries: the CPUs the machine has */
    uint64_t base_tsc, base_ns;
    uint64_t mult;          /* ns per TSC count, times 2^shift */
    uint32_t shift;
    uint32_t rate_bound;    /* how far mult may be from the true rate against the rate reference,
                               in 2^-32 parts; CLOCK_RATE_UNMEASURED while it is as stated */
    uint64_t tsc_hz;        /* the rate mult stands for */
    uint64_t asleep_ns;
    int64_t wall_ns;
    uint64_t wall_from, wall_until;
    int64_t wall_slew;
    uint64_t wall_slew_mult;
    int64_t offset[];       /* by CPU index: what its TSC reads more than the clock's */
};

/* This CPU's TSC, read once every earlier instruction is done (RDTSCP waits for them, SDM Vol.
   2B) and before any later one begins (the LFENCE after it); *cpu (when given): this CPU's index,
   its TSC_AUX (arch/smp.c percpu_load). */
static inline uint64_t clock_tsc_read(uint32_t *cpu) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp; lfence" : "=a"(lo), "=d"(hi), "=c"(aux) : : "memory");
    if (cpu) *cpu = aux;
    return (uint64_t)hi << 32 | lo;
}

/* floor(a * b / d) through 128 bits, and the same rounded up: false when it does not fit 64 bits
   (or d is 0). Time's ratios (counts to ns, rates and their bounds) need the 128-bit quotient,
   and the kernel links no routine for it: DIV divides the 128-bit RDX:RAX by a 64-bit operand
   when the quotient fits (SDM Vol. 2A DIV, else #DE, which the test before it rules out). */
static inline bool clock_muldiv_rem(uint64_t a, uint64_t b, uint64_t d, uint64_t *q, uint64_t *rem) {
    unsigned __int128 n = (unsigned __int128)a * b;
    uint64_t high = (uint64_t)(n >> 64), r;
    if (!d || high >= d) return false;
    __asm__("divq %4" : "=a"(*q), "=d"(r) : "a"((uint64_t)n), "d"(high), "rm"(d) : "cc");
    if (rem) *rem = r;
    return true;
}
static inline bool clock_muldiv(uint64_t a, uint64_t b, uint64_t d, uint64_t *q) {
    return clock_muldiv_rem(a, b, d, q, NULL);
}
static inline bool clock_muldiv_up(uint64_t a, uint64_t b, uint64_t d, uint64_t *q) {
    uint64_t r;
    if (!clock_muldiv_rem(a, b, d, q, &r)) return false;
    if (r && ++*q == 0) return false;
    return true;
}

/* CPU 0 at boot, after the ACPI tables and the page allocator, before any other CPU starts and
   before alarms come (they are set here and come once CPU 0's wake device is up). */
void clock_init(void);
/* A hypervisor's word for its guest's TSC rate (its CPUID leaf or MSR, vm/), before clock_init:
   the clock runs at it as stated (the first one registered), the CPU stating none there; source
   names it in the log. */
void clock_register_tsc_hz(uint64_t hz, const char *source);

/* A rate reference: a count of a known rate. Its owner reads the count between two readings of
   one CPU's TSC (a sample), runs it on past the device's wraps, and gives each sample to
   clock_rate_sample at the moment the clock asked for (or later). */
struct clock_rate_reference {
    const char *name;        /* in the log */
    uint64_t hz_num, hz_den; /* its count's rate: hz_num / hz_den a second */
    /* After a sleep, from clock_resume (CPU 0, interrupts off, the other CPUs stopped): its count
       may have stopped or begun again; it begins a new one and gives a sample of it. */
    void (*resume)(void);
};
struct clock_rate_sample {
    uint64_t tsc_a, tsc_b; /* the CPU's TSC (clock_tsc_read) just before and just after the count was taken */
    uint32_t cpu;          /* that CPU's index, as clock_tsc_read gives it */
    uint32_t jitter;       /* counts by which the count may be early or late */
    uint64_t count;
};
/* A sample of ref, interrupts off, any CPU but in no NMI: the moment (the monotonic clock's ns)
   the clock wants the next one at, CLOCK_NEVER when it wants none (a hypervisor states the rate).
   A sample of another reference than the last one's begins the windows on it. */
uint64_t clock_rate_sample(const struct clock_rate_reference *ref, const struct clock_rate_sample *s);

/* A wall-time reference's word: by the monotonic clock's moment `by`, the time (Unix ns) was at
   least unix_ns. The wall clock moves on by what it proves beyond what the references proved
   before (never back: a wall clock ahead is clock_adjust_wall's to slow), unless clock_set_wall
   or clock_adjust_wall stepped it since they began (at boot, or at a wake). After a wake, until
   the clock's search for the RTC's next second ends, what it proves beyond the time before the
   sleep is time asleep, which the boot clock takes too (and the sleeps on it are set again,
   wait_boot_clock_moved). Returns what the clocks moved on by (ns).
   Any context but an NMI, holding no spinlock. */
int64_t clock_wall_proof(int64_t unix_ns, uint64_t by);
/* The CPUs starting (arch/smp.c) compare their TSCs with each other and with the CPU starting
   them, all at once: it calls clock_cpus_compare with the count of CPUs comparing (itself among
   them) once every one has called clock_cpu_compare, after its LAPIC is up. Each returns with its
   offset in the clock's data. */
void clock_cpu_compare(void);
void clock_cpus_compare(unsigned cpus);

/* The monotonic clock; 0 before clock_init. It takes no lock and may be read in any context, an
   NMI's included: a reader waits only while another CPU is inside a write (a few field stores
   with its interrupts off, the longest a store per CPU of the offsets), and one on the writing
   CPU itself (an NMI on top of the write) reads the fields as they were before it. */
uint64_t clock_now_ns(void);
/* The boot clock, read as the monotonic one; 0 before clock_init. */
uint64_t clock_boot_ns(void);

/* The kernel stops (a panic), before the CPU that reports it halts the others: one halted inside
   a write would hold every reader; from here on readers take the fields as they were before a
   write in progress instead of waiting for it. */
void clock_stop(void);
uint64_t clock_tsc_hz(void); /* the TSC's rate as the clock has it now, 0 before clock_init */
/* The deadline ns from now (CLOCK_NEVER past the end). */
static inline uint64_t clock_after(uint64_t ns) {
    uint64_t now = clock_now_ns();
    return ns >= CLOCK_NEVER - now ? CLOCK_NEVER : now + ns;
}
/* ns pass on the clock with the CPU spinning, any context. Only for the short waits of a few us
   that a device's specification puts between two register accesses, where nothing can wake the
   CPU (a reset's settling time, a power change); any longer or conditional wait is an alarm or
   an interrupt. Callers that wait longer here are drivers' defects, not uses. */
static inline void clock_spin(uint64_t ns) {
    for (uint64_t until = clock_after(ns); clock_now_ns() < until;) __asm__ volatile("pause");
}
/* This CPU's TSC reading at which the monotonic clock reaches ns (the LAPIC's TSC-deadline mode
   arms on it; a program's clock starts from it); false past the TSC's reach. The answer holds
   until the rate changes (alarm_clock_changed tells the wake devices). */
bool clock_tsc_at(uint64_t ns, uint64_t *tsc);
/* The clock's data: its first frame (physical, *first) and the count of frames. */
size_t clock_data_pages(uint64_t *first);
/* S3 and hibernation, on CPU 0 with the others stopped: before sleeping, interrupts off; after
   waking, before anything reads the clock (the TSC may have begun again from 0, the rate
   reference's count may have stopped). The monotonic clock goes on from where it stopped, the
   rate's windows begin again on the reference's new count; the boot and wall clocks take the time
   asleep from the RTC (at least what it proves, refined once the RTC's next second is found).
   clock_resume returns the whole seconds asleep. */
void clock_suspend(void);
int64_t clock_resume(void);

int64_t clock_wall_ns(void);
int64_t clock_wall_seconds(void); /* rounded down */
void clock_set_wall(int64_t unix_ns); /* a step to unix_ns */
/* The wall clock moved by offset_ns over time instead of at once: it runs fast or slow by
   slew_ppm (0 < slew_ppm < 1000000, so it never goes back) until the whole offset is in; with
   slew_ppm 0 or past that, a step. A new adjustment takes over from where the last one got. */
void clock_adjust_wall(int64_t offset_ns, uint32_t slew_ppm);

struct tm_parts {
    int year, mon, mday, hour, min, sec, wday;
};
void time_to_parts(int64_t t, struct tm_parts *p);
int64_t time_from_parts(const struct tm_parts *p);
