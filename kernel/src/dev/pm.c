/* Power lifecycle (dev/pm.h): one thread takes every power change through
   apps -> disks -> devices -> firmware, and sleep through disks -> CPUs -> devices -> firmware
   and back. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pm.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "gui/wm.h"
#include "sys/sched.h"
#include "arch/ioapic.h"
#include "arch/smp.h"
#include "arch/wake.h"
#include "dev/pci.h"
#include "dev/timer.h"
#include "dev/serial.h"

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
    /* sleep is never forced and never replaces a shutdown that is already waiting */
    if (!stopping && !(action == PM_SLEEP && (busy || requested))) {
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
        for (struct pm_device *d = stages[s]; d; d = d->next)
            if (d->quiesce) {
                kprintf("pm: stopping %s\n", d->name);
                d->quiesce(d->ctx);
            }
    if (action == PM_REBOOT) power_reboot();
    power_off();
    for (;;) hlt();
}

/* ---- sleep (S3) ---- */
static const char *sleep_blocker(void) {
    if (!acpi_sleep_supported(3)) return "the firmware has no S3";
    if (!wake_available()) return "no wake page below 1 MiB";
    for (int s = 0; s < PM_STAGES; s++)
        for (struct pm_device *d = stages[s]; d; d = d->next)
            if (!d->resume) return d->name;
    return NULL;
}

static bool can_sleep; /* decided once by pm_sleep_check: the start menu asks on every redraw */

bool pm_can_sleep(void) { return can_sleep; }

void pm_sleep_check(void) {
    const char *why = sleep_blocker();
    can_sleep = !why;
    if (why) kprintf("pm: sleep unavailable (%s)\n", why);
    else kprintf("pm: sleep (S3) available\n");
}

static void enter_s3(void) { acpi_sleep_enter(3); }

static void sleep_now(void) {
    const char *why = sleep_blocker();
    /* a driver in an app (dev/pciuser.c) is not told about sleep yet: its device would come back
       reset under it */
    for (int i = 0; i < pci_count() && !why; i++)
        if (pci_at(i)->claimed) why = "An app drives one of the PCI devices";
    for (int s = 0; s < PM_STAGES && !why; s++)
        for (struct pm_device *d = stages[s]; d && !why; d = d->next)
            if (d->refuse) why = d->refuse(d->ctx);
    if (why) kprintf("pm: not sleeping: %s\n", why);
    if (why) {
        wm_notify("Cannot sleep", why);
        return;
    }
    kprintf("pm: going to sleep\n");
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (d->flush && d->flush(d) < 0) kprintf("pm: %s: cache flush failed\n", d->name);
    }
    bool aps = smp_suspend();
    bool slept = false;
    if (aps && acpi_sleep_prepare(3)) {
        pci_suspend();
        uint64_t f = irq_save();
        for (int s = 0; s < PM_STAGES; s++)
            for (struct pm_device *d = stages[s]; d; d = d->next)
                if (d->quiesce) d->quiesce(d->ctx);
        uint32_t pic = pic_state();
        ioapic_suspend();
        timer_suspend();
        slept = wake_sleep(enter_s3);
        /* awake (or never asleep): the CPU is back; now what it talks to */
        lapic_resume();
        pic_restore(pic);
        serial_init(); /* the log port, reset with the rest */
        ioapic_resume();
        int64_t secs = timer_resume();
        sched_clock_resume();
        acpi_wake_prepare(3);
        pci_resume();
        irq_restore(f);
        acpi_wake(3);
        for (int s = PM_STAGES - 1; s >= 0; s--)
            for (struct pm_device *d = stages[s]; d; d = d->next) d->resume(d->ctx);
        if (slept) kprintf("pm: awake after %ld s\n", (long)secs);
    }
    smp_resume();
    if (!slept) kprintf("pm: sleep failed%s\n", aps ? "" : " (a CPU did not stop)");
    wm_repaint();
    if (!slept) wm_notify("Sleep failed", aps ? "The firmware did not sleep" : "A processor did not stop");
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
        if (action == PM_SLEEP) {
            kprintf("pm: sleep requested\n");
            sleep_now(); /* apps keep running: they are only told the time moved */
        } else {
            kprintf("pm: %s requested%s\n", action == PM_REBOOT ? "restart" : "shutdown",
                    forced ? "" : " (apps may keep their windows)");
        }
        if (action != PM_SLEEP && close_windows()) {
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
        busy = false;
        /* a shutdown asked for while asleep (the power button wakes the machine) still happens */
        if (action != PM_SLEEP || wanted == PM_SLEEP) forced = requested = false;
        irq_restore(f);
    }
}

void pm_init(void) { kthread_create("power", pm_thread, NULL); }
