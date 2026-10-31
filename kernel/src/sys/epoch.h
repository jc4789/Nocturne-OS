/* Epochs (sys/epoch.c): memory that readers on any CPU may still be looking at without a lock
   goes back once every one of them is done with it.

   A reader reads between epoch_enter and epoch_leave, which only hold preemption off: no lock, no
   write, no shared count. Such a section never sleeps, never calls the scheduler, and is never in
   an interrupt's handler (nor an NMI's or a machine check's). A writer that takes something out
   of readers' reach (a handle table's old array, a closed handle) hands it to epoch_defer, which
   gives it back (calls its free) once every CPU has passed a quiet point since: a point at which
   the CPU is in no reader's section. The scheduler passes one at the head of every switch
   (switch_once), on every way back to ring 3 (sched_user_return) and as its idle loop begins to
   wait (sched_idle): epoch_quiet, sys/sched.c.

   Grace periods. One begins (numbered on from the last) when something deferred needs it, and ends
   once every CPU that was out of idle when it began has passed a quiet point; what was deferred
   before it began goes back after it ended. The CPUs are counted in the groups that share a
   last-level cache (topology's llc, as many as the machine has): a group counts its CPUs out of
   idle (active) and, for the grace period it is in, those of them that have not passed a quiet
   point yet (owing). A group whose count reaches 0 is paid; the grace period ends when every group
   is. A quiet point at which nothing is owed reads one word and its own CPU's; the counts are
   lines each group's own CPUs share, and the machine's count is changed once a group.

   Why a grace period ends:
   - A CPU running threads passes a quiet point whenever it switches threads, preempted or not,
     and whenever it goes back to ring 3 (from a system call, an exception or an interrupt).
   - An idle CPU passes none, and is not waited for. Its idle loop's quiet point (epoch_quiet with
     idle, at the top of the loop, interrupts off, before it parks or looks at its flag) pays what
     it owes and takes it out of its group's active ones, and a grace period that begins while it
     waits does not count it. Its next quiet point out of idle counts it in again (active), owing
     nothing for the grace period then running: what that one gives back was out of reach before
     it began, and so before the group took the CPU in (the group's lock orders the two). That
     point is the head of its next switch, before the thread it switches to runs a line; a switch
     that finds nothing to run counts it in and the loop's top takes it out again. From its idle
     loop's quiet point to that one the CPU reads nothing epoch protects: its idle loop does not,
     nor do the handlers of the interrupts it takes as it waits (no reader is in a handler).
   - The boot thread is the first CPU's idle thread, but runs the kernel's start and waits
     (sys/wait.c's sleep_in) where its CPU has nothing else to run: it idles there between two
     looks, out of the count in the same way (epoch_quiet with idle before the idling, without
     after it, before it goes on), rather than holding a grace period until a kick.
   - A CPU that never started is idle (each CPU's state starts so), and so is one that parked for
     a sleep: it parks from its idle loop after the loop's quiet point, and starts again in its
     idle loop after the sleep, still out. One left out at the start (too late for smp_init's
     deadline) never runs anything. Every CPU but the first comes in at its first switch, before
     anything it runs can read; the first is counted from epoch_init on, since the kernel's start
     runs on it.
   - A CPU that passes no quiet point for long (one thread computing in ring 3 where no interrupt
     comes, there being no tick; a long stretch in the kernel) is made to: a grace period not
     ended EPOCH_FORCE_NS after it began has each CPU that still owes it told a switch is due
     (sched_resched) and kicked (cpu_kick). In ring 3 the kick's interrupt goes back through
     sched_user_return; in the kernel the CPU switches where preemption is next on (at the
     kick's own way out when nothing holds it off, else as preempt_enable or irq_restore lets it),
     and a section that holds preemption off is bounded like any other. Either way it passes a
     quiet point, so every CPU counted pays within EPOCH_FORCE_NS and that bound.
   Nothing waits at a quiet point, and nothing looks at the CPUs at an interval: the quiet point
   that pays the last group ends the grace period and wakes the workers waiting for it.

   The workers: each group has a kernel thread that takes what its CPUs deferred, begins the grace
   period it needs, waits for its end and gives it back. A free runs in that thread, so it may
   sleep. */
#pragma once
#include "kernel.h"
#include "arch/preempt.h"

/* Embedded in what is deferred; its memory stays the deferrer's until free is called with it. */
struct epoch_item {
    struct epoch_item *next;
    void (*free)(struct epoch_item *item);
    uint64_t gp; /* the grace period it waits for */
};

static inline void epoch_enter(void) {
    ASSERT(!in_interrupt());
    preempt_disable();
}
static inline void epoch_leave(void) { preempt_enable(); }

/* Any context but a run queue's lock held, item out of every reader's reach: free(item) is called,
   in a kernel thread, once every reader's section that may have reached it has ended. */
void epoch_defer(struct epoch_item *item, void (*free)(struct epoch_item *item));
/* The scheduler's quiet point, preemption off (it is switching, going back to ring 3, or idle):
   this CPU is in no reader's section. idle: it is about to wait, and the CPU reads nothing epoch
   protects until its next quiet point without idle (the head of its next switch, or the boot
   thread's own after its idling). It may wake a worker: no run queue's lock held, and a caller
   that is about to wait looks at sched_resched after it. */
void epoch_quiet(bool idle);
/* At boot, after sched_init, before the first switch: the groups and their workers, and the boot
   CPU counted in. */
void epoch_init(void);
