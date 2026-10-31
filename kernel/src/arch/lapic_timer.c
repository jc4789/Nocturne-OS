/* The wake device (arch/lapic_timer.h). The LAPIC timer interrupts this CPU once:

   - in TSC-deadline mode where the CPU has it (CPUID 01h ECX[24]; SDM Vol. 3A 13.5.4.1): armed
     with the TSC reading at which the clock reaches the moment (clock_tsc_at), exact; when the
     clock's rate changes, alarm_clock_changed has every CPU arm it again;
   - else counting down once from an initial count at divide-by-1 (APM Vol. 2 16.4.1: one-shot,
     started by writing the initial count, stopped by writing 0), at most 2^32 - 1 counts ahead
     (a later alarm sets it again when it has come). The count is the TSC counts to the moment
     times this CPU's ratio of timer counts to TSC counts, a bound below the true ratio, so the
     interrupt comes at or before the moment, never after it. The ratio is measured against the
     TSC (not the clock: it does not change when the clock's rate does), first over a short
     window as the CPU starts, then raised by every count the timer is seen to have made since it
     was armed: each arming is a measurement whose lower bound replaces the ratio when higher.

   The timer counts at a rate that does not vary with P-states or C-states, in steps of up to 8
   (PPR 55898 2.1.12: "the APIC timer may increment in units of between 1 and 8"); it goes on in
   every C-state where CPUID 06h EAX[2] (ARAT) says so (PPR 55898, CPUID_Fn00000006_EAX;
   cpu_features.arat): where it does not, an idle CPU may not enter a C-state that stops it while
   an alarm is armed. */
#include "kernel.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/lapic_timer.h"
#include "arch/smp.h"
#include "sys/clock.h"

#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0
#define LAPIC_DIVIDE_BY_1 0xB
#define IA32_TSC_DEADLINE 0x6E0
#define LVT_TSC_DEADLINE (2u << 17)
#define COUNT_STEP 8         /* PPR 55898 2.1.12 */
#define RATIO_SHIFT 48       /* the ratio's fixed point */
#define RATIO_ONE (1ull << RATIO_SHIFT)
/* The first measurement runs until the TSC has gone 2^10 times the readings' width and the timer
   2^10 times its step: its bound is then within about 2^-8 of the true ratio, so an alarm a
   millisecond ahead comes at most a few us early, and the later armings close the rest. */
#define CALIBRATE_SHIFT 10

static enum { WAKE_ONE_SHOT, WAKE_DEADLINE } mode;
static int wake_vector;

PERCPU static struct {
    uint64_t ratio;       /* timer counts per TSC count, times 2^48: below the true one */
    uint64_t armed_tsc;   /* the TSC just before the initial count was written; 0: not counting */
    uint32_t armed_count;
} wake;

/* This CPU's timer against its TSC, masked: the ratio's first bound, 0 when it does not count.
   The timer's count is read between two TSC readings, so what it counted (less a step) over the
   TSC from just before its start to just after the reading bounds the ratio from below. A timer
   that has not counted once the TSC is past its window, or that stops counting while the window
   doubles, is none. */
static uint64_t calibrate(void) {
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIVIDE_BY_1);
    lapic_write(LAPIC_LVT_TIMER, LVT_UNUSED);
    uint64_t start_before = clock_tsc_read(NULL);
    lapic_write(LAPIC_TIMER_INIT, UINT32_MAX);
    uint64_t start_after = clock_tsc_read(NULL);
    uint64_t check = 0, counted_at_check = 0, before, after, counted;
    bool dead = false;
    for (;;) {
        before = clock_tsc_read(NULL);
        uint32_t current = lapic_read(LAPIC_TIMER_CUR);
        after = clock_tsc_read(NULL);
        counted = UINT32_MAX - current;
        if (!current) break; /* ran out: the whole count is the measure */
        uint64_t window = ((start_after - start_before) + (after - before)) << CALIBRATE_SHIFT;
        uint64_t elapsed = before - start_after;
        if (elapsed < window) continue;
        if (counted >= (uint64_t)COUNT_STEP << CALIBRATE_SHIFT) break;
        if (check && elapsed < 2 * check) continue;
        if (counted <= counted_at_check) { /* nothing at the window's end, or nothing since */
            dead = true;
            break;
        }
        check = elapsed;
        counted_at_check = counted;
    }
    lapic_write(LAPIC_TIMER_INIT, 0);
    uint64_t ratio;
    if (dead || counted <= COUNT_STEP ||
        !clock_muldiv(counted - COUNT_STEP, RATIO_ONE, after - start_before, &ratio))
        return 0;
    return ratio;
}

bool lapic_wake_init(int vector) {
    if (vector < 32 || vector >= 0xFF) return false;
    if (!wake_vector) {
        mode = cpu_features.tsc_deadline ? WAKE_DEADLINE : WAKE_ONE_SHOT;
        wake_vector = vector;
        if (mode == WAKE_DEADLINE)
            kprintf("apic: alarms by TSC deadline; the LAPIC timer %s in deep C-states\n",
                    cpu_features.arat ? "runs" : "stops");
    }
    if (mode == WAKE_DEADLINE) {
        /* in x2APIC mode the CPU keeps this write before the deadline's, no fence (SDM 13.5.4.1) */
        lapic_write(LAPIC_LVT_TIMER, LVT_TSC_DEADLINE | (uint32_t)vector);
        wrmsr(IA32_TSC_DEADLINE, 0);
        return true;
    }
    if (!this_cpu(wake).ratio) {
        uint64_t ratio = calibrate();
        if (!ratio) {
            kprintf("apic: cpu%u's LAPIC timer does not count\n", cpu_current_index());
            return false;
        }
        this_cpu(wake).ratio = ratio;
        if (cpu_current_index() == 0) {
            uint64_t hz = 0;
            clock_muldiv(ratio, clock_tsc_hz(), RATIO_ONE, &hz);
            kprintf("apic: alarms by one-shot count; LAPIC timer at least %lu kHz against the TSC; it %s in deep C-states\n",
                    hz / 1000, cpu_features.arat ? "runs" : "stops");
        }
    }
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIVIDE_BY_1);
    lapic_write(LAPIC_LVT_TIMER, (uint32_t)vector); /* one-shot, unmasked */
    lapic_write(LAPIC_TIMER_INIT, 0);
    this_cpu(wake).armed_tsc = 0;
    return true;
}

/* What the timer counted since it was armed raises the ratio where its bound is higher: the count
   it made (less a step) over the TSC from before its arming to after this reading. A timer that
   stopped meanwhile (a C-state without ARAT) gives a lower bound than the one held, which then
   stays. */
static void measure_armed(void) {
    uint64_t armed = this_cpu(wake).armed_tsc;
    if (!armed) return;
    uint32_t current = lapic_read(LAPIC_TIMER_CUR);
    uint64_t now = clock_tsc_read(NULL);
    uint64_t counted = this_cpu(wake).armed_count - (uint64_t)current, ratio;
    if (counted > COUNT_STEP && now > armed &&
        clock_muldiv(counted - COUNT_STEP, RATIO_ONE, now - armed, &ratio) && ratio > this_cpu(wake).ratio)
        this_cpu(wake).ratio = ratio;
}

void lapic_wake_at(uint64_t ns) {
    uint64_t tsc;
    if (mode == WAKE_DEADLINE) {
        /* 0 disarms it: never, or past the TSC's reach; a moment at TSC 0 is long past */
        if (ns == CLOCK_NEVER || !clock_tsc_at(ns, &tsc)) tsc = 0;
        else if (!tsc) tsc = 1;
        wrmsr(IA32_TSC_DEADLINE, tsc);
        return;
    }
    measure_armed();
    if (ns == CLOCK_NEVER) {
        lapic_write(LAPIC_TIMER_INIT, 0);
        this_cpu(wake).armed_tsc = 0;
        return;
    }
    bool reach = clock_tsc_at(ns, &tsc);
    uint64_t now = clock_tsc_read(NULL), count = UINT32_MAX;
    if (reach && !clock_muldiv(tsc > now ? tsc - now : 0, this_cpu(wake).ratio, RATIO_ONE, &count))
        count = UINT32_MAX;
    if (count > UINT32_MAX) count = UINT32_MAX;
    if (!count) count = 1;
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
    this_cpu(wake).armed_tsc = now;
    this_cpu(wake).armed_count = (uint32_t)count;
}
