/* Alarms (sys/alarm.c): a function called once the clock (sys/clock.h) has reached a moment, never
   before it. There is no periodic tick: each CPU keeps the alarms armed on it in its own heap,
   under its own lock, and its wake device (the LAPIC timer, arch/apic.c) interrupts it at the
   earliest one. A CPU with no alarm due is not interrupted at all, idle or busy.

   An alarm fires in the timer interrupt of the CPU it was armed on, interrupts off: fire may take
   spinlocks, wake tasks (sched_make_ready, wq_wake_all) and set the alarm again (a periodic one
   sets itself from its own at, so its period does not drift), but never sleeps, copies user
   memory or takes a mutex. regs is the interrupted context (the profiler samples it).

   The alarm's owner keeps it alive while it is armed or firing: alarm_cancel, after which neither
   is so, before the memory goes. An alarm is armed in one place at a time; setting an armed one
   moves it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct regs;
struct alarm {
    uint64_t at; /* clock_now_ns at which it fires (read by its owner, set by alarm_set) */
    void (*fire)(struct alarm *a, struct regs *r);
    /* alarm.c's: its place in its CPU's heap (a pairing heap: prev is the parent of a first child,
       else the sibling before) */
    struct alarm *child, *next, *prev;
    uint32_t armed_on; /* the index + 1 of the CPU whose heap holds it, 0 when not armed */
    uint32_t firing;   /* CPU index + 1 while fire runs there */
};

/* Zeroed memory is an alarm not armed; alarm_init gives it its function. */
static inline void alarm_init(struct alarm *a, void (*fire)(struct alarm *, struct regs *)) {
    *a = (struct alarm){.fire = fire};
}
/* Any context, any CPU: armed on this CPU for at (in the past: at once). */
void alarm_set(struct alarm *a, uint64_t at);
/* Any context but the timer interrupt of another alarm's fire that this one's fire waits for:
   not armed any more, and not firing on another CPU once it returns (it waits for that fire to
   end, so the caller holds no lock that fire takes). True when it was still armed. Called from
   its own fire, it only disarms. */
bool alarm_cancel(struct alarm *a);
static inline bool alarm_armed(const struct alarm *a) { return __atomic_load_n(&a->armed_on, __ATOMIC_RELAXED) != 0; }

void alarm_init_cpu(void);          /* each CPU as it starts, after its LAPIC: its heap and wake device */
void alarm_interrupt(struct regs *r); /* VEC_TIMER (arch/idt.c) */
/* Sleep (arch/smp.c): a CPU about to stop gives its alarms to CPU 0, interrupts off. */
void alarm_cpu_leave(void);
/* S3, on CPU 0: the wake device again after the clock is back (clock_resume). */
void alarm_resume(void);
