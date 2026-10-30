/* Power lifecycle: every power change goes through one kernel thread, in one order.

   Shut down, restart:
     1. the apps: every window gets EV_CLOSE and time to save, then every app
        that is still running is stopped;
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
   not come back, which the user is told. */
#pragma once
#include <stdbool.h>

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
    const char *(*refuse)(void *ctx); /* NULL, or why it cannot sleep right now (asked at each sleep) */
    void *ctx;
    bool cold_ok; /* without resume: as it was after the next boot (hibernation) */
    bool down;    /* pm.c: quiesced, not yet resumed */
    struct pm_device *next;
};

enum pm_action { PM_POWER_OFF, PM_REBOOT, PM_SLEEP, PM_HIBERNATE };

void pm_init(void);
void pm_register(struct pm_device *d); /* any CPU; d stays owned by the driver for good */
/* Ask for a power change. From a menu (interactive), apps that keep a window
   open cancel a shut down or restart; asking again within a minute closes them
   anyway. Otherwise (an app, the host) apps get a few seconds and are then
   stopped. Sleep closes nothing. Never blocks: any CPU, any context, also an
   interrupt handler or under a spinlock other than the wait queues'. */
void pm_request(enum pm_action action, bool interactive);
bool pm_stopping(void); /* a power change has passed the point of no return */
/* Whether sleep can be offered: the firmware has S3, there is a wake page, every registered
   device can resume. pm_sleep_check logs it once all the drivers have registered. */
bool pm_can_sleep(void);
bool pm_can_hibernate(void); /* C: on a disk, room below 1 MiB, every device back after it */
void pm_sleep_check(void);
/* Interrupts off: every registered device stops, stage by stage. NULL, or the first device that
   did not stop (with stop_at_failure, the ones after it are left running). */
const char *pm_quiesce_devices(bool stop_at_failure);
