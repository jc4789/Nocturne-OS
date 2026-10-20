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

   Quiesce callbacks run on the BSP with interrupts off: they may only poll
   (io_wait, the TSC), never sleep or wait for uptime_ms, which stands still.
   Resume callbacks run on the BSP with interrupts on, before any other thread:
   they may poll uptime_ms but must not sleep. The device lost its state; its
   PCI configuration space is already back (dev/pci.c). */
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
    void (*quiesce)(void *ctx); /* NULL: nothing to stop */
    void (*resume)(void *ctx);  /* NULL: cannot come back from sleep (no sleep while it is there) */
    const char *(*refuse)(void *ctx); /* NULL, or why it cannot sleep right now (asked at each sleep) */
    void *ctx;
    struct pm_device *next;
};

enum pm_action { PM_POWER_OFF, PM_REBOOT, PM_SLEEP };

void pm_init(void);
void pm_register(struct pm_device *d); /* d stays owned by the driver for good */
/* Ask for a power change. From a menu (interactive), apps that keep a window
   open cancel a shut down or restart; asking again within a minute closes them
   anyway. Otherwise (an app, the host) apps get a few seconds and are then
   stopped. Sleep closes nothing. Never blocks. */
void pm_request(enum pm_action action, bool interactive);
bool pm_stopping(void); /* a power change has passed the point of no return */
/* Whether sleep can be offered: the firmware has S3, there is a wake page, every registered
   device can resume. pm_sleep_check logs it once all the drivers have registered. */
bool pm_can_sleep(void);
void pm_sleep_check(void);
