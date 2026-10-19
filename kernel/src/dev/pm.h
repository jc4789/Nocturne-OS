/* Power lifecycle: every power change (shut down, restart) goes through one
   kernel thread, in one order:

     1. the apps: every window gets EV_CLOSE and time to save, then every app
        that is still running is stopped;
     2. the disks: every block device writes its cache back;
     3. the devices: each registered device stops its DMA and interrupts,
        stage by stage (PM_STAGE_IO first, PM_STAGE_BUS last);
     4. the firmware turns the power off or resets the machine.

   Quiesce callbacks run on the BSP with interrupts off: they may only poll
   (io_wait, the TSC), never sleep or wait for uptime_ms, which stands still. */
#pragma once
#include <stdbool.h>

enum pm_stage {
    PM_STAGE_IO,      /* network cards, audio: DMA that only moves data */
    PM_STAGE_STORAGE, /* disk controllers: shutdown notification after the flush */
    PM_STAGE_BUS,     /* controllers other devices sit behind (USB) */
    PM_STAGES
};

struct pm_device {
    const char *name;
    enum pm_stage stage;
    void (*quiesce)(void *ctx);
    void *ctx;
    struct pm_device *next;
};

enum pm_action { PM_POWER_OFF, PM_REBOOT };

void pm_init(void);
void pm_register(struct pm_device *d); /* d stays owned by the driver for good */
/* Ask for a power change. From a menu (interactive), apps that keep a window
   open cancel it; asking again within a minute closes them anyway. Otherwise
   (an app, the host) apps get a few seconds and are then stopped. Never blocks. */
void pm_request(enum pm_action action, bool interactive);
bool pm_stopping(void); /* a power change has passed the point of no return */
