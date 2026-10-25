/*
 * The monitor's mode (screenmode.h). The display driver knows what its monitor and display engine
 * can show; which of it the screen shows is the system's to say, and the user's: by default the
 * monitor's preferred mode (its native resolution), the user's choice for that monitor once made,
 * and a mode the user tries goes back by itself unless it is kept, so a mode the monitor cannot
 * show never leaves the screen dark. The driver shows each wish at its size and gives the screen
 * that size (n_pci_give_screen; the desktop follows it, wm_screen_resized), and says what it showed:
 * a wish it could not show is refused, and the next best one comes instead.
 *
 * A driver may give the screen back while it runs (a mode its display did not show): the
 * firmware's mode is shown again, and while its modes stay offered a mode tried asks it
 * to show that one again. What decides the desktop's size, as the Settings app is told it (struct
 * screen_modes source), tells that apart from a driver that ended.
 *
 * Under lock: everything below but shown_mhz. The trial's end is watched by a kernel thread of its
 * own.
 */
#include "gui/screenmode.h"
#include "gui/wm.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "display_abi.h"

#define REFUSED_MAX N_SCREEN_MODES_MAX /* the monitor's modes its driver could not show: each of them at most */
#define CHOICES_MAX 8u  /* monitors the user chose a mode for */
#define FALLBACK_MHZ 60000u

static struct mutex lock;
static struct n_screen_modes offered;  /* count 0: no driver offers modes */
/* The driver that offered modes last (dev/pciuser.c): none yet, still running (whether or not its
   modes are still offered), or ended (screen_modes_gone). */
static enum { DRIVER_NONE, DRIVER_RUNNING, DRIVER_ENDED } driver;
static uint32_t shown_mhz;             /* atomic: the refresh of the mode the driver says it shows */
static struct n_screen_mode wanted;    /* the last wish (all 0: none) */
static uint64_t wish_serial;           /* its number (struct n_screen_state mode_serial) */
static struct n_screen_mode refused[REFUSED_MAX];
static unsigned nrefused;
static bool refused_last;              /* the mode tried last could not be shown */
static struct {
    char monitor[sizeof offered.monitor];
    struct n_screen_mode mode;
} choices[CHOICES_MAX];
static unsigned nchoices, next_choice;
/* A mode on trial: trial_from comes back at trial_until (uptime_ms) unless it is kept. Changed on
   the BSP with interrupts on; the watch thread looks with interrupts off. */
static bool trial_on;
static uint64_t trial_until;
static struct n_screen_mode trial_from;
static struct wait_queue trial_wq;
static bool watch_started;

static bool same(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz;
}

static const struct n_screen_mode *offered_mode(const struct n_screen_mode *m) {
    for (uint32_t i = 0; i < offered.count; i++)
        if (same(&offered.modes[i], m)) return &offered.modes[i];
    return NULL;
}

static bool is_refused(const struct n_screen_mode *m) {
    for (unsigned i = 0; i < nrefused; i++)
        if (same(&refused[i], m)) return true;
    return false;
}

static void refuse(const struct n_screen_mode *m) {
    if (!is_refused(m) && nrefused < REFUSED_MAX) refused[nrefused++] = *m;
}

static void unrefuse(const struct n_screen_mode *m) {
    for (unsigned i = 0; i < nrefused; i++)
        if (same(&refused[i], m)) {
            refused[i] = refused[--nrefused];
            return;
        }
}

static const struct n_screen_mode *choice_for(const char *monitor) {
    for (unsigned i = 0; i < nchoices; i++)
        if (!strcmp(choices[i].monitor, monitor)) return &choices[i].mode;
    return NULL;
}

static void choose_for(const char *monitor, const struct n_screen_mode *m) {
    unsigned i = 0;
    while (i < nchoices && strcmp(choices[i].monitor, monitor)) i++;
    if (i == nchoices) {
        if (nchoices < CHOICES_MAX) i = nchoices++;
        else i = next_choice++ % CHOICES_MAX; /* the oldest goes */
        memcpy(choices[i].monitor, monitor, sizeof choices[i].monitor);
    }
    choices[i].mode = (struct n_screen_mode){m->width, m->height, m->refresh_mhz, 0};
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* The offered, not refused mode of width x height whose refresh is nearest mhz (the higher of two
   as near); any size when width is 0: the largest. NULL when there is none. */
static const struct n_screen_mode *nearest(uint32_t width, uint32_t height, uint32_t mhz) {
    const struct n_screen_mode *best = NULL;
    for (uint32_t i = 0; i < offered.count; i++) {
        const struct n_screen_mode *m = &offered.modes[i];
        if ((width && (m->width != width || m->height != height)) || is_refused(m)) continue;
        if (best) {
            uint64_t area = (uint64_t)m->width * m->height, best_area = (uint64_t)best->width * best->height;
            if (!width && area != best_area) {
                if (area < best_area) continue;
            } else {
                uint32_t d = distance(m->refresh_mhz, mhz), bd = distance(best->refresh_mhz, mhz);
                if (d > bd || (d == bd && m->refresh_mhz <= best->refresh_mhz)) continue;
            }
        }
        best = m;
    }
    return best;
}

/* The mode the screen shows when nothing is on trial: the user's choice for the monitor, else the
   monitor's preferred mode, else its preferred size at another refresh, else its largest size
   near 60 Hz; without the desktop (the text console keeps the firmware's size), the firmware's
   size first. All 0 when every mode was refused. */
static struct n_screen_mode pick(void) {
    const struct n_screen_mode *m = choice_for(offered.monitor), *preferred = NULL;
    if (m) m = offered_mode(m);
    if (m && !is_refused(m)) return *m;
    for (uint32_t i = 0; i < offered.count && !preferred; i++)
        if (offered.modes[i].flags & N_SCREEN_MODE_PREFERRED) preferred = &offered.modes[i];
    uint32_t mhz = preferred ? preferred->refresh_mhz : FALLBACK_MHZ;
    m = NULL;
    if (!wm_running()) m = nearest(fb.width, fb.height, mhz);
    if (!m && preferred) m = nearest(preferred->width, preferred->height, mhz);
    if (!m) m = nearest(0, 0, FALLBACK_MHZ);
    if (m) return *m;
    return (struct n_screen_mode){0};
}

static void mode_text(const struct n_screen_mode *m, char *out, size_t cap) {
    if (!m->width) ksnprintf(out, cap, "no mode");
    else ksnprintf(out, cap, "%ux%u at %u.%03u Hz", m->width, m->height, m->refresh_mhz / 1000, m->refresh_mhz % 1000);
}

/* A new wish, numbered anew even for the same mode again (the driver acts on every number);
   the driver hears of it once lock is dropped (publish). */
static void wish(const struct n_screen_mode *m, const char *why) {
    wanted = m->width ? (struct n_screen_mode){m->width, m->height, m->refresh_mhz, m->flags} : (struct n_screen_mode){0};
    wish_serial++;
    char text[48];
    mode_text(&wanted, text, sizeof text);
    kprintf("screen: %s: %s\n", why, text);
}

/* The wish as it is now to the driver's state; outside lock (the WM's comes after it). */
static void publish(void) {
    mutex_lock(&lock);
    struct n_screen_mode m = wanted;
    uint64_t serial = wish_serial;
    mutex_unlock(&lock);
    wm_screen_mode(&m, serial);
}

static void trial_end(void) {
    trial_on = false;
    wq_wake_all(&trial_wq);
}

static void trial_watch(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t irq = irq_save();
        uint64_t now = uptime_ms();
        bool due = trial_on && now >= trial_until;
        if (!trial_on) wq_wait(&trial_wq);
        else if (!due) wq_wait_timeout(&trial_wq, trial_until - now);
        irq_restore(irq);
        if (!due) continue;
        mutex_lock(&lock);
        bool back = trial_on && uptime_ms() >= trial_until;
        if (back) {
            trial_end();
            wish(&trial_from, "the mode on trial was not kept; back to");
        }
        mutex_unlock(&lock);
        if (back) publish();
    }
}

void screen_modes_offer(const struct n_screen_modes *m) {
    mutex_lock(&lock);
    /* What its head shows: kept while the driver offers modes and says no other. */
    if (!m->count) __atomic_store_n(&shown_mhz, 0, __ATOMIC_RELAXED);
    else if (m->shown.width) __atomic_store_n(&shown_mhz, m->shown.refresh_mhz, __ATOMIC_RELAXED);
    if (!m->count) {
        if (offered.count) kprintf("screen: the display driver drives no monitor any more\n");
        offered.count = 0;
        trial_end();
        nrefused = 0;
        if (wanted.width) wish(&(struct n_screen_mode){0}, "no display driver offers modes");
        mutex_unlock(&lock);
        publish();
        return;
    }
    driver = DRIVER_RUNNING;
    bool new_monitor = !offered.count || strncmp(offered.monitor, m->monitor, sizeof offered.monitor);
    offered = *m;
    offered.monitor[sizeof offered.monitor - 1] = 0;
    offered.name[sizeof offered.name - 1] = 0;
    if (offered.count > N_SCREEN_MODES_MAX) offered.count = N_SCREEN_MODES_MAX;
    if (m->done == 0) {
        /* Offered anew (a monitor connected, the driver started): it acts on the next wish. */
        if (new_monitor) {
            kprintf("screen: monitor %s%s%s, %u modes\n", offered.monitor, offered.name[0] ? ", " : "", offered.name,
                    offered.count);
            nrefused = 0;
            trial_end();
            refused_last = false;
        }
        bool keep = !new_monitor && wanted.width && offered_mode(&wanted) && !is_refused(&wanted);
        if (keep) wish(&wanted, "the monitor is offered again; it shows");
        else {
            struct n_screen_mode p = pick();
            wish(&p, "the monitor shows");
        }
    } else if (m->done == wish_serial && wanted.width && !same(&m->shown, &wanted)) {
        /* The driver could not show the wish: the one before a trial comes back, else the next best. */
        char text[48];
        mode_text(&wanted, text, sizeof text);
        kprintf("screen: the display driver could not show %s\n", text);
        refuse(&wanted);
        refused_last = true;
        if (trial_on) {
            trial_end();
            wish(&trial_from, "back to");
        } else {
            struct n_screen_mode p = pick();
            wish(p.width ? &p : &m->shown, "instead it shows");
        }
    }
    /* A result of the wish (or of an older one: the driver acts on the newer one next) changes nothing. */
    mutex_unlock(&lock);
    publish();
}

void screen_modes_gone(void) {
    static const struct n_screen_modes none;
    mutex_lock(&lock);
    driver = DRIVER_ENDED;
    mutex_unlock(&lock);
    screen_modes_offer(&none);
}

uint32_t screen_shown_refresh_mhz(void) { return __atomic_load_n(&shown_mhz, __ATOMIC_RELAXED); }

/* What the Settings app is shown without a driver's modes, or while the firmware's mode is shown:
   the firmware's monitor and mode (the scanout shown, its refresh unknown when it is a driver's). */
static void firmware_part(struct n_screen_modes *m, bool boot) {
    if (!m->count) {
        memset(m, 0, sizeof *m);
        memcpy(m->monitor, fb.monitor, sizeof fb.monitor);
        memcpy(m->name, fb.monitor_name, sizeof fb.monitor_name);
    }
    if (!boot && m->count) return;
    uint32_t w, h;
    fb_screen_size(&w, &h);
    m->shown = (struct n_screen_mode){w, h, boot ? fb.refresh_mhz : 0, 0};
}

int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op) {
    if (op == SCREEN_MODES_GET) {
        if (size != sizeof(struct screen_modes)) return -N_ERR_INVALID;
        struct screen_modes *out = kmalloc(sizeof *out);
        if (!out) return -N_ERR_NO_MEMORY;
        memset(out, 0, sizeof *out);
        bool remote = wm_remote_viewing(), boot = fb_boot_shown();
        mutex_lock(&lock);
        out->offered = offered;
        out->wanted = wanted;
        uint64_t now = uptime_ms();
        out->trial_ms = trial_on ? (uint32_t)(trial_until > now ? trial_until - now : 0) : 0;
        out->refused = refused_last;
        out->source = remote  ? SCREEN_SOURCE_REMOTE
                      : !boot ? SCREEN_SOURCE_DRIVER
                      : driver == DRIVER_RUNNING ? SCREEN_SOURCE_HANDED_BACK
                      : driver == DRIVER_ENDED   ? SCREEN_SOURCE_DRIVER_STOPPED
                                                 : SCREEN_SOURCE_FIRMWARE;
        mutex_unlock(&lock);
        firmware_part(&out->offered, boot);
        int64_t r = copy_to_user(user, out, sizeof *out);
        kfree(out);
        return r;
    }
    if (op != SCREEN_MODE_SET || size != sizeof(struct screen_mode_set)) return -N_ERR_INVALID;
    struct screen_mode_set s;
    if (copy_from_user(&s, user, sizeof s)) return -N_ERR_BAD_ADDRESS;
    s.monitor[sizeof s.monitor - 1] = 0;
    int64_t r = 0;
    bool changed = false;
    mutex_lock(&lock);
    if (s.how == SCREEN_MODE_SAVED) {
        if (!s.monitor[0] || !s.mode.width || !s.mode.height || !s.mode.refresh_mhz) r = -N_ERR_INVALID;
        else {
            choose_for(s.monitor, &s.mode);
            const struct n_screen_mode *m = offered.count && !strcmp(offered.monitor, s.monitor) ? offered_mode(&s.mode) : NULL;
            if (m && !trial_on && !same(m, &wanted)) {
                unrefuse(m);
                wish(m, "the user's choice for this monitor");
                changed = true;
            }
        }
    } else if (s.how == SCREEN_MODE_TRY) {
        const struct n_screen_mode *m = offered.count ? offered_mode(&s.mode) : NULL;
        if (!offered.count) r = -N_ERR_NOT_FOUND;
        else if (!m) r = -N_ERR_INVALID;
        else {
            if (!trial_on) trial_from = wanted;
            trial_on = true;
            trial_until = uptime_ms() + SCREEN_MODE_TRIAL_MS;
            refused_last = false;
            unrefuse(m);
            wish(m, "on trial");
            changed = true;
            if (!watch_started) {
                watch_started = true;
                kthread_create("screen mode", trial_watch, NULL);
            }
            wq_wake_all(&trial_wq);
        }
    } else if (s.how == SCREEN_MODE_KEEP || s.how == SCREEN_MODE_BACK) {
        if (!offered.count || !trial_on) r = -N_ERR_NOT_FOUND;
        else {
            trial_end();
            if (s.how == SCREEN_MODE_KEEP) {
                choose_for(offered.monitor, &wanted);
                char text[48];
                mode_text(&wanted, text, sizeof text);
                kprintf("screen: the user keeps %s\n", text);
            } else {
                wish(&trial_from, "the user goes back to");
                changed = true;
            }
        }
    } else r = -N_ERR_INVALID;
    mutex_unlock(&lock);
    if (changed) publish();
    return r;
}
