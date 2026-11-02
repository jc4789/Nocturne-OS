/* Power lifecycle (dev/pm.h): one thread takes every power change through
   apps -> disks -> devices -> firmware, and sleep through disks -> CPUs -> devices -> firmware
   and back. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/apic.h"
#include "arch/spin.h"
#include "dev/pm.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "fs/store.h"
#include "dev/hibernate.h"
#include "dev/iommu.h"
#include "sys/process.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "arch/ioapic.h"
#include "arch/smp.h"
#include "arch/wake.h"
#include "dev/pci.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "dev/serial.h"
#include "sys/log.h"
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

static const struct pm_desktop *desktop; /* atomic */

void pm_set_desktop(const struct pm_desktop *d) { __atomic_store_n(&desktop, d, __ATOMIC_RELEASE); }

/* ---- what is told ----
   The state the watch reports, under pm_lock. */
static struct n_power_state told;
static struct watched told_watched;

static void say(enum n_power_stage stage, enum pm_action action, enum n_power_reason reason, const char *detail,
                bool forceable) {
    uint64_t f = spin_lock(&pm_lock);
    told.serial++;
    told.stage = stage;
    told.action = action;
    told.reason = reason;
    told.forceable = forceable;
    strlcpy(told.detail, detail ? detail : "", sizeof told.detail);
    spin_unlock(&pm_lock, f);
    watched_changed(&told_watched);
}

int64_t pm_watch_syscall(void) { return watched_open(&told_watched); }

int64_t pm_state_syscall(void *out, size_t size) {
    if (size != sizeof told) return -N_ERR_INVALID;
    struct n_power_state now;
    uint64_t f = spin_lock(&pm_lock);
    now = told;
    spin_unlock(&pm_lock, f);
    now.can_sleep = pm_can_sleep();
    now.can_hibernate = pm_can_hibernate();
    return copy_to_user(out, &now, sizeof now);
}

/* Every window gets EV_CLOSE; an app may ask its user to save first. Returns
   false when a menu request is cancelled by an app that kept its window. */
static bool close_windows(void) {
    const struct pm_desktop *d = __atomic_load_n(&desktop, __ATOMIC_ACQUIRE);
    if (!d) return true;
    char title[64], shown[64] = "";
    int left = d->window_count(title, sizeof title);
    if (!left) return true;
    kprintf("pm: asking %d windows to close\n", left);
    d->close_windows();
    uint64_t start = clock_now_ns(), progress = start;
    int last = left;
    bool shown_forced = false;
    while ((left = d->window_count(title, sizeof title)) > 0) {
        uint64_t now = clock_now_ns();
        if (left < last) { last = left; progress = now; }
        if (is_forced() ? now - start >= GRACE_NS : now - progress >= STALL_NS) break;
        if (now - start >= NS_PER_S && (strcmp(title, shown) || shown_forced != is_forced())) {
            shown_forced = is_forced();
            say(N_POWER_CLOSING, wanted_now(), N_POWER_REASON_NONE, title, !shown_forced);
            strlcpy(shown, title, sizeof shown);
        }
        sleep_until(clock_after(50 * NS_PER_MS));
    }
    if (!left || is_forced()) return true;
    say(N_POWER_CANCELLED, wanted_now(), N_POWER_REASON_NONE, title, false);
    kprintf("pm: cancelled, \"%s\" kept its window\n", title);
    uint64_t f = spin_lock(&pm_lock);
    force_until = clock_after(FORCE_NS);
    spin_unlock(&pm_lock, f);
    return false;
}

static void stop_apps(void) {
    uint64_t end = clock_after(STOP_NS);
    int left;
    while ((left = process_stop_all()) && clock_now_ns() < end) sleep_until(clock_after(20 * NS_PER_MS));
    if (left) kprintf("pm: %d app tasks did not end\n", left);
}

static bool flush_disks(void) {
    bool ok = true;
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        if (__atomic_load_n(&d->gone, __ATOMIC_ACQUIRE)) continue; /* what it held left with it */
        /* blk logs it; what a disk's cache lost before (blk_cache_lost) a sleep does not make worse */
        if (blk_persist(d) < 0 && !__atomic_load_n(&d->cache_lost, __ATOMIC_ACQUIRE)) ok = false;
    }
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
   off are its own CPU's: the other CPUs still run their kernel threads while the devices stop, and
   the hold keeps those from the disks. */
static NORETURN void finish(enum pm_action action) {
    blk_hold();
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
/* For the log. */
static const char *reason_text(enum n_power_reason reason) {
    switch (reason) {
    case N_POWER_REASON_NONE: return "none";
    case N_POWER_REASON_IOMMU: return "the IOMMU's state is unconfirmed, or it did not come back";
    case N_POWER_REASON_NO_S3: return "the firmware has no S3";
    case N_POWER_REASON_NO_WAKE_PAGE: return "no wake page below 1 MiB";
    case N_POWER_REASON_DEVICE: return "a device cannot come back";
    case N_POWER_REASON_DEVICE_BUSY: return "a device cannot sleep now";
    case N_POWER_REASON_DEVICE_STUCK: return "a device did not stop";
    case N_POWER_REASON_APP_DRIVER: return "an app drives one of the PCI devices";
    case N_POWER_REASON_DISK_FLUSH: return "a disk did not write its cache back";
    case N_POWER_REASON_CPU_STOP: return "a processor did not stop";
    case N_POWER_REASON_FIRMWARE: return "the firmware did not sleep";
    case N_POWER_REASON_LOW_ROOM: return "no room below 1 MiB";
    case N_POWER_REASON_NOT_ON_DISK: return "C: is not on a disk";
    case N_POWER_REASON_NO_MEMORY: return "not enough memory";
    case N_POWER_REASON_NO_SPACE: return "not enough free space on C:";
    case N_POWER_REASON_IMAGE_TOO_LARGE: return "the image did not fit in memory or on C:";
    case N_POWER_REASON_STORE_WRITE: return "C: could not be written";
    default: return "unknown";
    }
}

static enum n_power_reason sleep_blocker(bool hibernate, const char **detail) {
    *detail = NULL;
    if (!iommu_power_ready()) return N_POWER_REASON_IOMMU;
    enum n_power_reason why = hibernate ? hibernate_blocker(detail) : !acpi_sleep_supported(3) ? N_POWER_REASON_NO_S3 : N_POWER_REASON_NONE;
    if (why) return why;
    if (!wake_available()) return N_POWER_REASON_NO_WAKE_PAGE;
    for (int s = 0; s < PM_STAGES; s++)
        for (struct pm_device *d = first_of(s); d; d = next_of(d))
            if (!d->resume && !(hibernate && d->cold_ok)) {
                *detail = d->name;
                return N_POWER_REASON_DEVICE;
            }
    return N_POWER_REASON_NONE;
}

/* decided once by pm_sleep_check: the start menu asks on every redraw */
static bool can_sleep, can_hibernate;

bool pm_can_sleep(void) { return can_sleep; }
bool pm_can_hibernate(void) { return can_hibernate; }

static void blocker_log(const char *what, enum n_power_reason why, const char *detail) {
    if (why) kprintf("pm: %s unavailable (%s%s%s)\n", what, reason_text(why), detail ? ": " : "", detail ? detail : "");
}

void pm_sleep_check(void) {
    const char *detail;
    enum n_power_reason why = sleep_blocker(false, &detail);
    can_sleep = !why;
    blocker_log("sleep", why, detail);
    if (!why) kprintf("pm: sleep (S3) available\n");
    why = sleep_blocker(true, &detail);
    can_hibernate = !why;
    blocker_log("hibernation", why, detail);
    if (!why) kprintf("pm: hibernation available\n");
    watched_changed(&told_watched);
}

static void enter_s3(void) { acpi_sleep_enter(3); }

/* Devices, interrupt controllers and the CPU down, enter(), and all of it back. True after a real
   sleep (S3 woke up, or a hibernation image was restored), false when enter() returned or a device
   did not stop (*why and *detail say which; enter() is then not called). On CPU 0, every other CPU parked
   (smp_suspend): from the freeze to the thaw no other thread runs, the devices are down or not
   back yet. The interrupts this saves and restores are CPU 0's own. */
static bool machine_sleep(void (*enter)(void), unsigned state, enum pm_action action, enum n_power_reason *why,
                          const char **detail, bool *lost_told) {
    acpi_freeze(true);
    const char *stuck = pci_suspend();
    uint64_t f = irq_save();
    if (!stuck) stuck = pm_quiesce_devices(true);
    irq_suspend();
    /* the log's readers are told again in log_resume, once the APICs are back; before the clock
       stops, which bounds the serial port's last wait */
    log_suspend();
    alarm_suspend(); /* until its LAPIC is back: the clock's own alarms as it comes back wait */
    clock_suspend();
    /* called off, the way back is the one after an enter() that returned */
    bool slept = !stuck && wake_sleep(enter);
    /* awake (or never asleep): the CPU is back; now what it talks to */
    /* Wake firmware may have enabled plain routes. Stop their sources before any
       remapping mode transition; the saved routes remain intact for the final restore. */
    ioapic_mask_all();
    /* The clock first: every wait from here on (a unit's or a link's that does not answer) is
       bounded on it, and until it is back it stands still (the TSC began again from 0). */
    int64_t secs = clock_resume();
    /* before anything sends it an interrupt or a request, and before the APIC goes back to x2APIC
       mode, which comes after interrupt remapping with extended IDs (SDM 13.12.7); units that
       never slept are still on, translating with the claims' tables, and are left alone. Units
       that do not come back are turned off where they confirm it (iommu_resume): the IOAPICs'
       entries and the PCI functions' messages are then made again plain, or left off, and x2APIC
       mode comes back without remapping, the deviation arch/apic.c describes for the boot. */
    const char *units_lost = slept ? iommu_resume() : NULL;
    if (!apic_init_cpu()) panic("pm: this CPU's APIC did not come back in x2APIC mode");
    irq_resume(); /* the 8259 masked again, every IOAPIC entry written again */
    log_resume(); /* the serial port set up again, its interrupt routed again above */
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
    if (units_lost) say(N_POWER_DEVICE_LOST, action, N_POWER_REASON_IOMMU, units_lost, false);
    if (lost) say(N_POWER_DEVICE_LOST, action, N_POWER_REASON_DEVICE, lost, false);
    *lost_told = units_lost || lost;
    if (slept) kprintf("pm: awake after %ld s\n", (long)secs);
    if (stuck) {
        *why = N_POWER_REASON_DEVICE_STUCK;
        *detail = stuck;
    }
    return slept;
}

static void sleep_now(bool hibernate) {
    enum pm_action action = hibernate ? PM_HIBERNATE : PM_SLEEP;
    const char *detail;
    enum n_power_reason why = sleep_blocker(hibernate, &detail);
    /* a driver in an app (dev/pciuser.c) is not told about sleep yet: its device would come back
       reset under it */
    for (int i = 0; i < pci_count() && !why; i++)
        if (pci_at(i)->claimed) why = N_POWER_REASON_APP_DRIVER;
    for (int s = 0; s < PM_STAGES && !why; s++)
        for (struct pm_device *d = first_of(s); d && !why; d = next_of(d)) {
            const char *refused = d->refuse ? d->refuse(d->ctx) : NULL;
            if (!refused) continue;
            kprintf("pm: %s: %s\n", d->name, refused);
            why = N_POWER_REASON_DEVICE_BUSY;
            detail = d->name;
        }
    if (!why && hibernate) why = hibernate_prepare();
    if (why) {
        kprintf("pm: not %s: %s%s%s\n", hibernate ? "hibernating" : "sleeping", reason_text(why), detail ? ": " : "",
                detail ? detail : "");
        say(N_POWER_REFUSED, action, why, detail, false);
        return;
    }
    kprintf("pm: going to %s\n", hibernate ? "hibernate" : "sleep");
    store_sync(); /* what was saved is on its volume, whatever becomes of the sleep */
    /* no transfer is cut off by the controllers losing their state, and after a snapshot nothing
       but the image reaches the disks */
    blk_hold();
    bool parked = false, slept = false, lost_told = false;
    if (!flush_disks()) why = N_POWER_REASON_DISK_FLUSH; /* asleep or in an image, it could be lost */
    if (!why) {
        parked = true;
        if (!smp_suspend()) why = N_POWER_REASON_CPU_STOP;
    }
    if (why) {
    } else if (hibernate) {
        acpi_links_save();
        slept = machine_sleep(hibernate_snapshot, 4, action, &why, &detail, &lost_told);
        if (!slept && !why) {
            /* the snapshot: the machine as it is from here on never comes back */
            say(N_POWER_SAVING, action, N_POWER_REASON_NONE, NULL, false);
            if (!(why = hibernate_write())) {
                uint64_t f = spin_lock(&pm_lock);
                __atomic_store_n(&stopping, true, __ATOMIC_RELAXED);
                spin_unlock(&pm_lock, f);
                kprintf("pm: hibernated, power off\n");
                finish(PM_POWER_OFF);
            }
        }
    } else if (!acpi_sleep_prepare(3)) {
        why = N_POWER_REASON_FIRMWARE;
    } else if (!(slept = machine_sleep(enter_s3, 3, action, &why, &detail, &lost_told)) && !why) {
        why = N_POWER_REASON_FIRMWARE;
    }
    if (parked) smp_resume();
    blk_release();
    if (hibernate) hibernate_cleanup(slept);
    const struct pm_desktop *d = __atomic_load_n(&desktop, __ATOMIC_ACQUIRE);
    if (d) d->repaint();
    if (!slept) {
        kprintf("pm: %s failed: %s%s%s\n", hibernate ? "hibernation" : "sleep", reason_text(why), detail ? ": " : "",
                detail ? detail : "");
        say(N_POWER_FAILED, action, why, detail, false);
    } else if (!lost_told) {
        say(N_POWER_AWAKE, action, N_POWER_REASON_NONE, NULL, false);
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
            say(N_POWER_STOPPING, action, N_POWER_REASON_NONE, NULL, false);
            sleep_until(clock_after(TELL_NS)); /* whoever tells the user does, before the apps stop */
            stop_apps();
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

/* On the first CPU: the sleep it runs (smp_suspend, wake_sleep) comes back on the CPU the firmware
   wakes. */
void pm_init(void) {
    watched_init(&told_watched, NULL);
    kthread_create_on("power", pm_thread, NULL, 0);
}
