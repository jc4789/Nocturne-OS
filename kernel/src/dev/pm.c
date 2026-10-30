/* Power lifecycle (dev/pm.h): one thread takes every power change through
   apps -> disks -> devices -> firmware, and sleep through disks -> CPUs -> devices -> firmware
   and back. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/pm.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "fs/store.h"
#include "dev/hibernate.h"
#include "dev/iommu.h"
#include "gui/wm.h"
#include "sys/sched.h"
#include "sys/keep.h"
#include "arch/ioapic.h"
#include "arch/smp.h"
#include "arch/wake.h"
#include "dev/pci.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "dev/serial.h"
#include "vm/vm.h"

void power_off(void);
void power_reboot(void);

#define GRACE_NS   (5 * NS_PER_S)  /* not from a menu: time the apps get to close */
#define STALL_NS   (15 * NS_PER_S) /* from a menu: no window closed for this long cancels */
#define FORCE_NS   (60 * NS_PER_S) /* from a menu, after a cancel: asking again closes them anyway */
#define STOP_NS    (3 * NS_PER_S)  /* stopped apps get this long to end */

/* Each stage's list only grows: a device joins at its end with a release store, and the walks
   (any CPU, any time) follow the links with acquire loads, never under pm_lock. */
static struct pm_device *stages[PM_STAGES];
static struct wait_queue pm_wq;
/* The request and the power thread's progress: requested, busy, force_until, and every change of
   forced, stopping and wanted; also the appends to the stages' lists. pm_request comes from tasks
   on any CPU and from the power button's handler. Taken alone; only the wait queue's lock is taken
   under it. forced, stopping and wanted are also read without it (atomic loads): what the power
   thread or a caller does with a value a moment old is what it would have done just before. */
static struct spinlock pm_lock;
static bool requested, forced, busy, stopping;
static enum pm_action wanted;
static uint64_t force_until;

static struct pm_device *first_of(int stage) { return __atomic_load_n(&stages[stage], __ATOMIC_ACQUIRE); }
static struct pm_device *next_of(const struct pm_device *d) { return __atomic_load_n(&d->next, __ATOMIC_ACQUIRE); }
static bool is_forced(void) { return __atomic_load_n(&forced, __ATOMIC_RELAXED); }
static enum pm_action wanted_now(void) { return __atomic_load_n(&wanted, __ATOMIC_RELAXED); }

void pm_register(struct pm_device *d) {
    d->next = NULL;
    uint64_t f = spin_lock(&pm_lock);
    struct pm_device **pp = &stages[d->stage];
    while (*pp) pp = &(*pp)->next;
    __atomic_store_n(pp, d, __ATOMIC_RELEASE);
    spin_unlock(&pm_lock, f);
}

void pm_request(enum pm_action action, bool interactive) {
    uint64_t f = spin_lock(&pm_lock);
    /* sleep is never forced and never replaces a shutdown that is already waiting */
    bool nap = action == PM_SLEEP || action == PM_HIBERNATE;
    if (!stopping && !(nap && (busy || requested))) {
        __atomic_store_n(&wanted, action, __ATOMIC_RELAXED);
        /* asking again while apps are closing, or soon after a cancel, means "close them anyway" */
        if (!interactive || busy || clock_now_ns() < force_until) __atomic_store_n(&forced, true, __ATOMIC_RELAXED);
        requested = true;
        wq_wake_all(&pm_wq);
    }
    spin_unlock(&pm_lock, f);
}

bool pm_stopping(void) { return __atomic_load_n(&stopping, __ATOMIC_RELAXED); }

/* Every window gets EV_CLOSE; an app may ask its user to save first. Returns
   false when a menu request is cancelled by an app that kept its window. */
static bool close_windows(void) {
    char title[64], shown[64] = "";
    int left = wm_window_count(title, sizeof title);
    if (!left) return true;
    kprintf("pm: asking %d windows to close\n", left);
    wm_close_windows();
    uint64_t start = clock_now_ns(), progress = start, told = 0;
    int last = left;
    bool hint = false; /* the toast is narrow: the window's title and the way out take turns */
    while ((left = wm_window_count(title, sizeof title)) > 0) {
        uint64_t now = clock_now_ns();
        if (left < last) { last = left; progress = now; }
        if (is_forced() ? now - start >= GRACE_NS : now - progress >= STALL_NS) break;
        if (now - start >= NS_PER_S && (strcmp(title, shown) || now - told >= 3 * NS_PER_S)) {
            hint = !is_forced() && !strcmp(title, shown) && !hint;
            wm_notify(wanted_now() == PM_REBOOT ? "Waiting to restart" : "Waiting to shut down",
                      hint ? "Choose again to force it" : title);
            strlcpy(shown, title, sizeof shown);
            told = now;
        }
        sleep_until(clock_after(50 * NS_PER_MS));
    }
    if (!left || is_forced()) return true;
    wm_notify(wanted_now() == PM_REBOOT ? "Restart cancelled" : "Shutdown cancelled", title);
    kprintf("pm: cancelled, \"%s\" kept its window\n", title);
    uint64_t f = spin_lock(&pm_lock);
    force_until = clock_after(FORCE_NS);
    spin_unlock(&pm_lock, f);
    return false;
}

static void stop_apps(void) {
    uint64_t end = clock_after(STOP_NS);
    int left;
    while ((left = task_stop_apps()) && clock_now_ns() < end) sleep_until(clock_after(20 * NS_PER_MS));
    if (left) kprintf("pm: %d app tasks did not end\n", left);
}

static bool flush_disks(void) {
    bool ok = true;
    for (int i = 0; i < blk_count(); i++)
        if (blk_persist(blk_get(i)) < 0) ok = false; /* blk logs it */
    return ok;
}

const char *pm_quiesce_devices(bool stop_at_failure) {
    const char *stuck = NULL;
    for (int s = 0; s < PM_STAGES; s++)
        for (struct pm_device *d = first_of(s); d; d = next_of(d)) {
            d->down = true; /* reached: resumed after a sleep, also one called off from here */
            if (!d->quiesce || d->quiesce(d->ctx)) continue;
            kprintf("pm: %s did not stop\n", d->name);
            if (!stuck) stuck = d->name;
            if (stop_at_failure) return stuck;
        }
    return stuck;
}

/* The volume's saves are on the disks before (pm_thread): after a hibernation's snapshot nothing
   may be committed, the image has the machine as it was before any of it. The interrupts it turns
   off are its own CPU's: the other CPUs still run their kernel threads while the devices stop. */
static NORETURN void finish(enum pm_action action) {
    flush_disks(); /* a disk that fails is logged: the power goes off all the same */
    if (action == PM_POWER_OFF) acpi_sleep_prepare(5); /* the firmware's _PTS(5), while interrupts still run */
    cli();
    kprintf("pm: stopping the devices\n");
    pm_quiesce_devices(false);
    serial_flush();
    if (action == PM_REBOOT) power_reboot();
    power_off();
    for (;;) hlt();
}

/* ---- sleep (S3) and hibernation ---- */
static const char *sleep_blocker(bool hibernate) {
    if (!iommu_power_ready()) return "the IOMMU's state is unconfirmed, or it did not come back from a sleep";
    const char *why = hibernate ? hibernate_blocker() : !acpi_sleep_supported(3) ? "the firmware has no S3" : NULL;
    if (why) return why;
    if (!wake_available()) return "no wake page below 1 MiB";
    for (int s = 0; s < PM_STAGES; s++)
        for (struct pm_device *d = first_of(s); d; d = next_of(d))
            if (!d->resume && !(hibernate && d->cold_ok)) return d->name;
    return NULL;
}

/* decided once by pm_sleep_check: the start menu asks on every redraw */
static bool can_sleep, can_hibernate;

bool pm_can_sleep(void) { return can_sleep; }
bool pm_can_hibernate(void) { return can_hibernate; }

void pm_sleep_check(void) {
    const char *why = sleep_blocker(false);
    can_sleep = !why;
    if (why) kprintf("pm: sleep unavailable (%s)\n", why);
    else kprintf("pm: sleep (S3) available\n");
    why = sleep_blocker(true);
    can_hibernate = !why;
    if (why) kprintf("pm: hibernation unavailable (%s)\n", why);
    else kprintf("pm: hibernation available\n");
}

static void enter_s3(void) { acpi_sleep_enter(3); }

/* Devices, interrupt controllers and the CPU down, enter(), and all of it back. True after a real
   sleep (S3 woke up, or a hibernation image was restored), false when enter() returned or a device
   did not stop (*why says which; enter() is then not called). On CPU 0, every other CPU parked
   (smp_suspend): from the freeze to the thaw no other thread runs, the devices are down or not
   back yet. The interrupts this saves and restores are CPU 0's own. */
static bool machine_sleep(void (*enter)(void), unsigned state, const char **why) {
    acpi_freeze(true);
    const char *stuck = pci_suspend();
    uint64_t f = irq_save();
    if (!stuck) stuck = pm_quiesce_devices(true);
    uint32_t pic = pic_state();
    pic_mask_all();
    ioapic_suspend();
    clock_suspend();
    serial_flush(); /* the alarm that drains the log port does not come while asleep */
    /* called off, the way back is the one after an enter() that returned */
    bool slept = !stuck && wake_sleep(enter);
    /* awake (or never asleep): the CPU is back; now what it talks to */
    /* Wake firmware may have enabled plain routes. Stop their sources before any
       remapping mode transition; the saved routes remain intact for the final restore. */
    pic_mask_all();
    ioapic_mask_all();
    serial_init(); /* the log port, reset with the rest */
    /* The clock first: every wait from here on (a unit's or a link's that does not answer) is
       bounded on it, and until it is back it stands still (the TSC began again from 0). */
    int64_t secs = clock_resume();
    /* before anything sends it an interrupt or a request, and before the APICs go back to x2APIC
       mode, which comes after interrupt remapping with extended IDs (SDM 11.12.7); units that
       never slept are still on, translating with the claims' tables, and are left alone. Units
       that do not come back are turned off where they confirm it (iommu_resume): the IOAPICs'
       entries and the PCI functions' messages are then made again plain, or left off, and x2APIC
       mode comes back without remapping, the deviation arch/apic.c describes for the boot. */
    const char *units_lost = slept ? iommu_resume() : NULL;
    lapic_resume();
    ioapic_resume();
    if (slept) pic_restore(pic);
    else pic_restore_mask((uint16_t)pic); /* no reset occurred: do not erase a pending edge */
    vm_resume(); /* the hypervisor's other overlay pages and its interrupts, after the LAPIC */
    alarm_resume(); /* CPU 0's wake device, after its LAPIC */
    acpi_wake_prepare(state);
    pci_resume(slept, units_lost != NULL);
    irq_restore(f);
    /* a hibernation's snapshot is no sleep: the firmware heard of none */
    if (state == 3 || slept) acpi_wake(state);
    else acpi_links_forget();
    const char *lost = NULL;
    for (int s = PM_STAGES - 1; s >= 0; s--)
        for (struct pm_device *d = first_of(s); d; d = next_of(d)) {
            if (!d->down) continue; /* after a device that did not stop: it never stopped */
            d->down = false;
            if (!d->resume || d->resume(d->ctx)) continue;
            kprintf("pm: %s did not come back\n", d->name);
            if (!lost) lost = d->name;
        }
    acpi_freeze(false);
    if (units_lost) wm_notify("The IOMMU did not come back from sleep", units_lost);
    if (lost) wm_notify(slept ? "A device did not wake up" : "A device did not restart", lost);
    if (slept) kprintf("pm: awake after %ld s\n", (long)secs);
    if (stuck) {
        static char msg[64];
        ksnprintf(msg, sizeof msg, "%s did not stop", stuck);
        *why = msg;
    }
    return slept;
}

static void sleep_now(bool hibernate) {
    const char *why = sleep_blocker(hibernate);
    /* a driver in an app (dev/pciuser.c) is not told about sleep yet: its device would come back
       reset under it */
    for (int i = 0; i < pci_count() && !why; i++)
        if (pci_at(i)->claimed) why = "An app drives one of the PCI devices";
    for (int s = 0; s < PM_STAGES && !why; s++)
        for (struct pm_device *d = first_of(s); d && !why; d = next_of(d))
            if (d->refuse) why = d->refuse(d->ctx);
    if (!why && hibernate) hibernate_prepare(&why);
    if (why) {
        kprintf("pm: not %s: %s\n", hibernate ? "hibernating" : "sleeping", why);
        wm_notify(hibernate ? "Cannot hibernate" : "Cannot sleep", why);
        return;
    }
    kprintf("pm: going to %s\n", hibernate ? "hibernate" : "sleep");
    keep_flush(); /* a state changed a moment ago is kept too */
    store_sync(); /* what was saved is on its volume, whatever becomes of the sleep */
    /* no transfer is cut off by the controllers losing their state, and after a snapshot nothing
       but the image reaches the disks */
    blk_hold();
    bool parked = false, slept = false;
    if (!flush_disks()) why = "A disk did not write its cache back"; /* asleep or in an image, it could be lost */
    if (!why) {
        parked = true;
        if (!smp_suspend()) why = "A processor did not stop";
    }
    if (why) {
    } else if (hibernate) {
        acpi_links_save();
        slept = machine_sleep(hibernate_snapshot, 4, &why);
        if (!slept && !why) {
            /* the snapshot: the machine as it is from here on never comes back */
            wm_notify("Hibernating", "Saving the memory to C:");
            if (!(why = hibernate_write())) {
                uint64_t f = spin_lock(&pm_lock);
                __atomic_store_n(&stopping, true, __ATOMIC_RELAXED);
                spin_unlock(&pm_lock, f);
                kprintf("pm: hibernated, power off\n");
                finish(PM_POWER_OFF);
            }
        }
    } else if (!acpi_sleep_prepare(3) || !(slept = machine_sleep(enter_s3, 3, &why))) {
        if (!why) why = "The firmware did not sleep";
    }
    if (parked) smp_resume();
    blk_release();
    if (hibernate) hibernate_cleanup(slept);
    wm_repaint();
    if (!slept) {
        kprintf("pm: %s failed: %s\n", hibernate ? "hibernation" : "sleep", why);
        wm_notify(hibernate ? "Hibernation failed" : "Sleep failed", why);
    }
}

static void pm_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock(&pm_lock);
        while (!requested) {
            wq_wait_locked(&pm_wq, &pm_lock);
            spin_take(&pm_lock);
        }
        requested = false;
        busy = true;
        enum pm_action action = wanted;
        spin_unlock(&pm_lock, f);
        bool nap = action == PM_SLEEP || action == PM_HIBERNATE;
        if (nap) {
            kprintf("pm: %s requested\n", action == PM_SLEEP ? "sleep" : "hibernation");
            sleep_now(action == PM_HIBERNATE); /* apps keep running: they are only told the time moved */
        } else {
            kprintf("pm: %s requested%s\n", action == PM_REBOOT ? "restart" : "shutdown",
                    is_forced() ? "" : " (apps may keep their windows)");
        }
        if (!nap && close_windows()) {
            f = spin_lock(&pm_lock);
            __atomic_store_n(&stopping, true, __ATOMIC_RELAXED);
            action = wanted;
            spin_unlock(&pm_lock, f);
            wm_notify(action == PM_REBOOT ? "Restarting" : "Shutting down",
                      action == PM_REBOOT ? "See you in a moment..." : "Good night!");
            stop_apps();
            keep_flush(); /* a state changed a moment ago is kept too */
            store_sync(); /* what was saved reaches its volume first */
            finish(action);
        }
        f = spin_lock(&pm_lock);
        busy = false;
        /* a shutdown asked for while asleep (the power button wakes the machine) still happens */
        if (!nap || wanted == PM_SLEEP || wanted == PM_HIBERNATE) {
            __atomic_store_n(&forced, false, __ATOMIC_RELAXED);
            requested = false;
        }
        spin_unlock(&pm_lock, f);
    }
}

void pm_init(void) { kthread_create("power", pm_thread, NULL); }
