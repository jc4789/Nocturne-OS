/* Power lifecycle: every power change goes through one kernel thread, in one order. The kernel
   tells what happens (the power button, the lid, a wake, the host's request: struct n_power_events)
   and carries out what it is asked (SYS_POWER). What a press means, whether the machine may sleep,
   when it sleeps, and asking the windows to close and for how long is the machine service's
   (user/apps/machine.c).

   Shut down, restart:
     1. the apps: every app still running is stopped;
     2. the disks: every block device writes its cache back;
     3. the devices: each registered device stops its DMA and interrupts,
        stage by stage (PM_STAGE_IO first, PM_STAGE_BUS last);
     4. the firmware turns the power off or resets the machine.

   Sleep (ACPI S3, suspend to RAM): the apps keep running where they were.
     1. the disks write their caches back, the APs halt, the firmware's _PTS;
     2. the devices stop as above, then PCI configuration, interrupt controllers
        and the CPU are saved and the firmware turns everything but RAM off;
     3. on waking, the CPU, the interrupt controllers, the timer and PCI come
        back, then the firmware's _WAK, then each device's resume runs,
        PM_STAGE_BUS first, then the APs. Sleep is offered only when every
        registered device can resume.

   Hibernate (dev/hibernate.h): as sleep, but the firmware is not asked; RAM is
   written to C: and the power goes off. A device that cannot resume may
   still take part when it comes back by itself after a cold boot (cold_ok:
   the firmware's display).

   Quiesce callbacks run on one CPU with its interrupts off: they may only poll
   (io_wait, the clock: clock_spin and clock_now_ns, sys/clock.h), never sleep.
   For a sleep or hibernation that CPU is CPU 0 and every other CPU is parked;
   for a shut down or restart it is the power thread's, and the other CPUs
   still run kernel threads. They return false when the device did not
   confirm it stopped (its DMA may still run): a sleep or hibernation is then
   called off and the devices stopped so far are resumed; a shut down goes on.
   Resume callbacks run on CPU 0 with interrupts on, every other CPU still
   parked and no other thread running (the AML of _WAK is frozen too,
   dev/acpi.h): they may poll the clock but must not sleep. The device lost
   its state, or was only quiesced (a sleep called off); its PCI configuration
   space is already back (dev/pci.c). They return false when the device did
   not come back.

   Every step is typed state (struct n_power_state, abi.h) that a watch reports: the kernel says
   no sentence, and the shell (N_CAP_SHELL) tells the user. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "abi.h"

enum pm_stage {
    PM_STAGE_IO,      /* network cards, audio, input, display */
    PM_STAGE_STORAGE, /* disk controllers: shutdown notification after the flush */
    PM_STAGE_BUS,     /* controllers other devices sit behind (USB) */
    PM_STAGES
};

struct pm_device {
    const char *name;
    enum pm_stage stage;
    bool (*quiesce)(void *ctx); /* NULL: nothing to stop */
    bool (*resume)(void *ctx);  /* NULL: cannot come back from sleep (no sleep while it is there) */
    const char *(*refuse)(void *ctx); /* NULL, or (for the log) why it cannot sleep right now (asked at each sleep) */
    void *ctx;
    bool cold_ok; /* without resume: as it was after the next boot (hibernation) */
    bool down;    /* pm.c: quiesced, not yet resumed */
    struct pm_device *next;
};

enum pm_action { PM_POWER_OFF = POWER_OFF, PM_REBOOT = POWER_REBOOT, PM_SLEEP = POWER_SLEEP, PM_HIBERNATE = POWER_HIBERNATE };

void pm_init(void);
void pm_register(struct pm_device *d); /* any CPU; d stays owned by the driver for good */
/* Carry out a power change. A sleep never replaces a change already waiting. Never blocks: any CPU,
   any context under no spinlock other than the wait queues'. */
void pm_request(enum pm_action action);
/* What happened: told to the machine service through struct n_power_events. Any CPU, any context. */
void pm_button_pressed(void);
void pm_lid_is(bool closed);
void pm_host_asks(enum pm_action action);
/* Whether the machine can sleep: the firmware has S3, there is a wake page, every registered
   device can resume. pm_sleep_check logs it once all the drivers have registered. */
bool pm_can_sleep(void);
bool pm_can_hibernate(void); /* C: on a disk, room below 1 MiB, every device back after it */
void pm_sleep_check(void);
/* SYS_POWER_STATE and SYS_POWER_WATCH (N_CAP_SHELL checked by the caller). */
int64_t pm_state_syscall(void *out, size_t size);
int64_t pm_watch_syscall(void);
/* SYS_POWER_EVENTS, SYS_POWER_EVENTS_WATCH and SYS_POWER_SAY (N_CAP_POWER checked by the caller). */
int64_t pm_events_syscall(void *out, size_t size);
int64_t pm_events_watch_syscall(void);
int64_t pm_say_syscall(const void *state, size_t size);
/* Interrupts off: every registered device stops, stage by stage. NULL, or the first device that
   did not stop (with stop_at_failure, the ones after it are left running). */
const char *pm_quiesce_devices(bool stop_at_failure);
