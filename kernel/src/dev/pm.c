/* Power lifecycle (dev/pm.h): one thread takes every power change it is asked for through
   apps -> disks -> devices -> firmware, and sleep through disks -> CPUs -> devices -> firmware
   and back. What happens is told; what it means is the machine service's. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/apic.h"
#include "arch/spin.h"
#include "dev/pm.h"
#include "hal/hal.h"
#include "sys/syscall.h"
#include "sys/watch.h"
#include "dev/acpi.h"
#include "dev/blk.h"
#include "dev/fb.h"
#include "dev/input.h"
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

#define STOP_NS (3 * NS_PER_S) /* stopped apps get this long to end */

static struct wait_queue pm_wq;
/* The request and the power thread's progress (requested, busy, stopping, wanted), the state told and
   the events; also the appends to the stages' lists. Taken from tasks on any CPU and from the power
   button's handler. Taken alone; only the wait queue's lock is taken under it. */
static struct spinlock pm_lock;
static bool requested, busy, stopping, napping;
static enum pm_action wanted;

void pm_request(enum pm_action action) {
    uint64_t f = spin_lock(&pm_lock);
    /* sleep never replaces a shutdown that is already waiting */
    bool nap = action == PM_SLEEP || action == PM_HIBERNATE;
    if (!stopping && !(nap && (busy || requested))) {
        wanted = action;
        requested = true;
        wq_wake_all(&pm_wq);
    }
    spin_unlock(&pm_lock, f);
}

/* ---- what happened ----
   Counted under pm_lock; the machine service reads them and decides. */
static struct n_power_events happened;
static struct watched happened_watched;

void pm_button_pressed(void) {
    uint64_t f = spin_lock(&pm_lock);
    /* the press that woke the machine says nothing more */
    bool counts = !napping;
    if (counts) happened.button_presses++;
    spin_unlock(&pm_lock, f);
    if (counts) watched_changed(&happened_watched);
}

void pm_lid_is(bool closed) {
    uint64_t f = spin_lock(&pm_lock);
    bool changed = happened.lid_present && (happened.lid_closed != 0) != closed;
    happened.lid_present = 1;
    happened.lid_closed = closed;
    if (changed) happened.lid_changes++;
    spin_unlock(&pm_lock, f);
    if (changed) watched_changed(&happened_watched);
}

void pm_host_asks(enum pm_action action) {
    uint64_t f = spin_lock(&pm_lock);
    happened.host_asks++;
    happened.host_action = action;
    spin_unlock(&pm_lock, f);
    watched_changed(&happened_watched);
}

static void woke(void) {
    uint64_t f = spin_lock(&pm_lock);
    happened.wakes++;
    happened.last_wake_ns = clock_now_ns();
    spin_unlock(&pm_lock, f);
    watched_changed(&happened_watched);
}

int64_t pm_events_watch_syscall(void) { return watched_open(&happened_watched); }

int64_t pm_events_syscall(void *out, size_t size) {
    if (size != sizeof happened) return -N_ERR_INVALID;
    struct n_power_events now;
    uint64_t f = spin_lock(&pm_lock);
    now = happened;
    spin_unlock(&pm_lock, f);
    now.last_input_ns = input_last_ns();
    now.can_sleep = pm_can_sleep();
    now.can_hibernate = pm_can_hibernate();
    return copy_to_user(out, &now, sizeof now);
}

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

/* SYS_POWER_SAY: the stages the machine service leads, for whoever tells the user. */
int64_t pm_say_syscall(const void *state, size_t size) {
    struct n_power_state s;
    if (size != sizeof s) return -N_ERR_INVALID;
    if (copy_from_user(&s, state, sizeof s)) return -N_ERR_BAD_ADDRESS;
    if ((s.stage != N_POWER_CLOSING && s.stage != N_POWER_CANCELLED && s.stage != N_POWER_STOPPING) || s.action > POWER_HIBERNATE)
        return -N_ERR_INVALID;
    s.detail[sizeof s.detail - 1] = 0;
    say(s.stage, (enum pm_action)s.action, N_POWER_REASON_NONE, s.detail, s.forceable);
    return 0;
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

/* The volume's saves are on the disks before (pm_thread): after a hibernation's snapshot nothing
   may be committed, the image has the machine as it was before any of it. The interrupts it turns
   off are its own CPU's: the other CPUs still run their kernel threads while the devices stop, and
   the hold keeps those from the disks. */
static NORETURN void finish(enum pm_action action) {
    hal_power_begin();
    blk_hold();
    flush_disks(); /* a disk that fails is logged: the power goes off all the same */
    if (action == PM_POWER_OFF) acpi_sleep_prepare(5); /* the firmware's _PTS(5), while interrupts still run */
    cli();
    kprintf("pm: stopping the devices\n");
    hal_quiesce_all(false);
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
    return N_POWER_REASON_NONE;
}

/* The machine's part, decided once by pm_sleep_check: the start menu asks on every redraw. The
   devices' part follows the tree, which changes as drivers come and go. */
static bool can_sleep, can_hibernate;

bool pm_can_sleep(void) { return can_sleep && !hal_power_cannot(false); }
bool pm_can_hibernate(void) { return can_hibernate && !hal_power_cannot(true); }

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
   back yet. The interrupts this saves and restores are CPU 0's own. The other CPUs start again
   (smp_resume) once the interrupt controllers are back and before any device is: a device's
   interrupts go to any CPU, and one sent to a CPU still held in INIT is lost. */
static bool machine_sleep(void (*enter)(void), unsigned state, enum pm_action action, enum n_power_reason *why,
                          const char **detail, bool *lost_told) {
    acpi_freeze(true);
    const char *stuck = pci_suspend();
    uint64_t f = irq_save();
    if (!stuck) stuck = hal_quiesce_all(true);
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
    /* before anything sends it an interrupt or a request, and before the APIC goes back to its
       mode (arch/apic.h), x2APIC mode coming after interrupt remapping with extended IDs (SDM
       13.12.7); units that never slept are still on, translating with the claims' tables, and are
       left alone. Units that do not come back are turned off where they confirm it
       (iommu_resume): the IOAPICs' entries and the PCI functions' messages are then made again
       plain, or left off, and the APIC comes back in its mode without remapping. */
    const char *units_lost = slept ? iommu_resume() : NULL;
    if (!apic_init_cpu()) panic("pm: this CPU's APIC did not come back in %s mode", apic_x2() ? "x2APIC" : "xAPIC");
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
    smp_resume();
    const char *lost = hal_resume_all();
    input_resumed();
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
    hal_power_begin(); /* no driver comes or goes from here to the end, and none is on its way */
    if (!why) {
        enum hal_refusal refusal;
        const char *who = hal_power_refused(hibernate, &refusal);
        switch (refusal) {
        case HAL_REFUSAL_NONE: break;
        case HAL_REFUSAL_CANNOT: why = N_POWER_REASON_DEVICE; break;
        case HAL_REFUSAL_BUSY: why = N_POWER_REASON_DEVICE_BUSY; break;
        case HAL_REFUSAL_PROCESS: why = N_POWER_REASON_APP_DRIVER; break;
        }
        if (who) detail = who;
    }
    if (!why && (detail = hal_claims_suspend())) why = N_POWER_REASON_DEVICE_STUCK;
    bool claims_told = !why;
    if (!why && hibernate) why = hibernate_prepare();
    if (why) {
        kprintf("pm: not %s: %s%s%s\n", hibernate ? "hibernating" : "sleeping", reason_text(why), detail ? ": " : "",
                detail ? detail : "");
        say(N_POWER_REFUSED, action, why, detail, false);
        if (claims_told) hal_claims_resume();
        hal_power_end();
        return;
    }
    kprintf("pm: going to %s\n", hibernate ? "hibernate" : "sleep");
    store_sync(); /* what was saved is on its volume, whatever becomes of the sleep */
    /* no transfer is cut off by the controllers losing their state, and after a snapshot nothing
       but the image reaches the disks */
    blk_hold();
    bool parked = false, slept = false, lost_told = false, cpus_back = false; /* machine_sleep started them */
    if (!flush_disks()) why = N_POWER_REASON_DISK_FLUSH; /* asleep or in an image, it could be lost */
    if (!why) {
        parked = true;
        if (!smp_suspend()) why = N_POWER_REASON_CPU_STOP;
    }
    if (why) {
    } else if (hibernate) {
        acpi_links_save();
        slept = machine_sleep(hibernate_snapshot, 4, action, &why, &detail, &lost_told);
        cpus_back = true;
        if (!slept && !why) {
            /* the snapshot: the machine as it is from here on never comes back */
            say(N_POWER_SAVING, action, N_POWER_REASON_NONE, NULL, false);
            if (!(why = hibernate_write())) {
                uint64_t f = spin_lock(&pm_lock);
                stopping = true;
                spin_unlock(&pm_lock, f);
                kprintf("pm: hibernated, power off\n");
                finish(PM_POWER_OFF);
            }
        }
    } else if (!acpi_sleep_prepare(3)) {
        why = N_POWER_REASON_FIRMWARE;
    } else {
        slept = machine_sleep(enter_s3, 3, action, &why, &detail, &lost_told);
        cpus_back = true;
        if (!slept && !why) why = N_POWER_REASON_FIRMWARE;
    }
    if (parked && !cpus_back) smp_resume();
    blk_release();
    hal_claims_resume();
    hal_power_end();
    if (hibernate) hibernate_cleanup(slept);
    if (screen_display) screen_display->repaint();
    if (slept) {
        acpi_lid_read(); /* it may have changed while nothing listened */
        woke();
    }
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
        if (action == PM_SLEEP || action == PM_HIBERNATE) {
            kprintf("pm: %s requested\n", action == PM_SLEEP ? "sleep" : "hibernation");
            f = spin_lock(&pm_lock);
            napping = true;
            spin_unlock(&pm_lock, f);
            sleep_now(action == PM_HIBERNATE); /* apps keep running: they are only told the time moved */
            f = spin_lock(&pm_lock);
            napping = false;
            spin_unlock(&pm_lock, f);
        } else {
            kprintf("pm: %s requested\n", action == PM_REBOOT ? "restart" : "shutdown");
            f = spin_lock(&pm_lock);
            stopping = true;
            action = wanted;
            spin_unlock(&pm_lock, f);
            say(N_POWER_STOPPING, action, N_POWER_REASON_NONE, NULL, false);
            stop_apps();
            store_sync(); /* what was saved reaches its volume first */
            finish(action);
        }
        f = spin_lock(&pm_lock);
        busy = false;
        spin_unlock(&pm_lock, f);
    }
}

/* On the first CPU: the sleep it runs (smp_suspend, wake_sleep) comes back on the CPU the firmware
   wakes. */
void pm_init(void) {
    watched_init(&told_watched, NULL);
    watched_init(&happened_watched, NULL);
    kthread_create_on("power", pm_thread, NULL, 0);
}
