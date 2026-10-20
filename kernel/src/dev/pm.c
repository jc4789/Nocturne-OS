/* Power lifecycle (dev/pm.h): one thread takes every power change through
   apps -> disks -> devices -> firmware. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pm.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "gui/wm.h"
#include "sys/sched.h"

void power_off(void);
void power_reboot(void);

#define GRACE_MS   5000  /* not from a menu: time the apps get to close */
#define STALL_MS   15000 /* from a menu: no window closed for this long cancels */
#define FORCE_MS   60000 /* from a menu, after a cancel: asking again closes them anyway */
#define STOP_MS    3000  /* stopped apps get this long to end */

static struct pm_device *stages[PM_STAGES];
static struct wait_queue pm_wq;
static volatile bool requested, forced, busy, stopping;
static volatile enum pm_action wanted;
static uint64_t force_until;

void pm_register(struct pm_device *d) {
    uint64_t f = irq_save();
    struct pm_device **pp = &stages[d->stage];
    while (*pp) pp = &(*pp)->next;
    d->next = NULL;
    *pp = d;
    irq_restore(f);
}

void pm_request(enum pm_action action, bool interactive) {
    uint64_t f = irq_save();
    if (!stopping) {
        wanted = action;
        /* asking again while apps are closing, or soon after a cancel, means "close them anyway" */
        if (!interactive || busy || uptime_ms() < force_until) forced = true;
        requested = true;
        wq_wake_all(&pm_wq);
    }
    irq_restore(f);
}

bool pm_stopping(void) { return stopping; }

/* Every window gets EV_CLOSE; an app may ask its user to save first. Returns
   false when a menu request is cancelled by an app that kept its window. */
static bool close_windows(void) {
    char title[64], shown[64] = "";
    int left = wm_window_count(title, sizeof title);
    if (!left) return true;
    kprintf("pm: asking %d windows to close\n", left);
    wm_close_windows();
    uint64_t start = uptime_ms(), progress = start, told = 0;
    int last = left;
    bool hint = false; /* the toast is narrow: the window's title and the way out take turns */
    while ((left = wm_window_count(title, sizeof title)) > 0) {
        uint64_t now = uptime_ms();
        if (left < last) { last = left; progress = now; }
        if (forced ? now - start >= GRACE_MS : now - progress >= STALL_MS) break;
        if (now - start >= 1000 && (strcmp(title, shown) || now - told >= 3000)) {
            hint = !forced && !strcmp(title, shown) && !hint;
            wm_notify(wanted == PM_REBOOT ? "Waiting to restart" : "Waiting to shut down",
                      hint ? "Choose again to force it" : title);
            strlcpy(shown, title, sizeof shown);
            told = now;
        }
        sleep_ms(50);
    }
    if (!left || forced) return true;
    wm_notify(wanted == PM_REBOOT ? "Restart cancelled" : "Shutdown cancelled", title);
    kprintf("pm: cancelled, \"%s\" kept its window\n", title);
    force_until = uptime_ms() + FORCE_MS;
    return false;
}

static void stop_apps(void) {
    uint64_t end = uptime_ms() + STOP_MS;
    int left;
    while ((left = task_stop_apps()) && uptime_ms() < end) sleep_ms(20);
    if (left) kprintf("pm: %d app tasks did not end\n", left);
}

static NORETURN void finish(enum pm_action action) {
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (d->flush && d->flush(d) < 0) kprintf("pm: %s: cache flush failed\n", d->name);
    }
    if (action == PM_POWER_OFF) acpi_sleep_prepare(5); /* the firmware's _PTS(5), while interrupts still run */
    cli();
    for (int s = 0; s < PM_STAGES; s++)
        for (struct pm_device *d = stages[s]; d; d = d->next) {
            kprintf("pm: stopping %s\n", d->name);
            d->quiesce(d->ctx);
        }
    if (action == PM_REBOOT) power_reboot();
    power_off();
    for (;;) hlt();
}

static void pm_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        while (!requested) wq_wait(&pm_wq);
        requested = false;
        busy = true;
        irq_restore(f);
        enum pm_action action = wanted;
        kprintf("pm: %s requested%s\n", action == PM_REBOOT ? "restart" : "shutdown", forced ? "" : " (apps may keep their windows)");
        if (close_windows()) {
            f = irq_save();
            stopping = true;
            action = wanted;
            irq_restore(f);
            wm_notify(action == PM_REBOOT ? "Restarting" : "Shutting down",
                      action == PM_REBOOT ? "See you in a moment..." : "Good night!");
            stop_apps();
            finish(action);
        }
        f = irq_save();
        busy = forced = requested = false;
        irq_restore(f);
    }
}

void pm_init(void) { kthread_create("power", pm_thread, NULL); }
