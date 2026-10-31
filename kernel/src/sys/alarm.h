/* Alarms (sys/alarm.c): a function called once the monotonic clock (sys/clock.h) has reached a
   moment, never before it. There is no periodic tick: each CPU keeps the alarms armed on it in its
   own heap, under its own lock, and its wake device (the LAPIC timer, arch/lapic_timer.h)
   interrupts it at the earliest one. A CPU with no alarm due is not interrupted at all.

   An alarm may name a range [early, at] instead of a moment: it fires at the latest at `at`, and
   earlier when its CPU's wake device comes for the alarms before it and it is the next one, in
   its range by then, so alarms that need not be exact share interrupts.

   An alarm fires in a timer interrupt, interrupts off, normally on the CPU it was armed on: fire
   may take spinlocks, wake tasks (sched_make_ready, wq_wake_all) and set the alarm again (a
   periodic one sets itself from its own at, so its period does not drift), but never sleeps,
   copies user memory, takes a mutex or shoots down TLBs. regs is the interrupted context (the
   profiler samples it). One alarm's fire never runs on two CPUs at once: when it becomes due on
   one while its last fire still runs on another, that other runs it again once its fire returns.

   The alarm's owner keeps it alive while it is armed or firing: alarm_cancel, after which neither
   is so, before the memory goes. An alarm is armed in one place at a time, whoever sets it and
   from wherever at once; setting an armed one moves it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct regs;
struct alarm {
    uint64_t at;    /* the latest moment it fires (read by its owner; set by alarm_set*) */
    uint64_t early; /* the earliest */
    void (*fire)(struct alarm *a, struct regs *r);
    /* alarm.c's: its place in its CPU's heap (a pairing heap: prev is the parent of a first child,
       else the sibling before), where it is, and who runs it */
    struct alarm *child, *next, *prev;
    uint32_t armed_on; /* the CPU index + 1 of the heap holding it; 0 when not armed */
    uint32_t firing;   /* the CPU index + 1 whose fire runs it */
};

/* Zeroed memory is an alarm not armed; alarm_init gives it its function. */
static inline void alarm_init(struct alarm *a, void (*fire)(struct alarm *, struct regs *)) {
    *a = (struct alarm){.fire = fire};
}
/* Any context, any CPU: armed on this CPU for any moment in [early, latest] (in the past: at
   once). alarm_set is the range of one moment. */
void alarm_set_range(struct alarm *a, uint64_t early, uint64_t latest);
static inline void alarm_set(struct alarm *a, uint64_t at) { alarm_set_range(a, at, at); }
/* Any context but the timer interrupt of another alarm's fire that this one's fire waits for:
   not armed any more, and not firing on another CPU once it returns (it waits for that fire to
   end, with interrupts on where its caller had them on, so the caller holds no lock that fire
   takes). True when it was still armed. Called from its own fire, it only disarms. */
bool alarm_cancel(struct alarm *a);
static inline bool alarm_armed(const struct alarm *a) { return __atomic_load_n(&a->armed_on, __ATOMIC_RELAXED) != 0; }

void alarm_init_cpu(void);          /* each CPU as it starts, after its LAPIC: its heap and wake device */
/* The clock's rate changed (sys/clock.c, after it): every CPU sets its wake device again, since
   the TSC reading of each moment moved. Any context but holding a CPU's alarm lock. */
void alarm_clock_changed(void);
/* Sleep (arch/smp.c): a CPU about to stop gives its alarms to CPU 0, interrupts off. */
void alarm_cpu_leave(void);
/* S3 and hibernation, on CPU 0, interrupts off, before the clock stops: its wake device off until
   alarm_resume. Alarms set meanwhile (the clock's own as it comes back, before the LAPIC is) wait
   in its heap. */
void alarm_suspend(void);
/* S3, on CPU 0: the wake device again after its LAPIC is back (apic_init_cpu). */
void alarm_resume(void);
