/*
 * The monitors' modes (screenmode.h). A display driver knows what its monitor and display engine
 * can show; which of it a screen shows is the system's to say, and the user's: by default the
 * monitor's preferred mode (its native resolution), the user's choice for that monitor once made,
 * and a mode the user tries goes back by itself unless it is kept, so a mode the monitor cannot
 * show never leaves the screen dark. The driver shows each wish at its size and gives the screen
 * buffers of that size (n_pci_give_screen; the desktop follows it), and says what it showed: a
 * wish it could not show is refused, and the next best one comes instead.
 *
 * A driver may give screen 0 back while it runs (a mode its display did not show): the firmware's
 * mode is shown again, and while its modes stay offered a mode tried asks it to show that one
 * again. What decides a screen's size, as the Settings app is told it (struct screen_modes
 * source), tells that apart from a driver that ended.
 *
 * Each screen has its own choice (struct choice): the modes offered, the wish, the refusals and a
 * trial. The mode the user keeps for each monitor is held here until the machine stops: the settings
 * service hears of each change (N_DOMAIN_DISPLAY) and keeps it, and applies it back at the start.
 * Until then a screen shows its monitor's preferred mode. Every change of what SYS_SCREEN_INFO
 * reports of the modes tells its watches (wm_screen_info_changed).
 *
 * Under lock (a mutex, any CPU; the WM's lock and a screen's use come after it): everything below.
 * The trials' ends are watched by a kernel thread of its own, which sleeps until the first of them
 * while trial_changes still reads the same.
 */
#include "gui/screenmode.h"
#include "gui/wm.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "sys/clock.h"
#include "sys/settings.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "display_abi.h"

#define REFUSED_MAX N_SCREEN_MODES_MAX /* the monitor's modes its driver could not show: each of them at most */

struct choice {
    struct n_screen_modes offered;  /* count 0: no driver offers modes */
    /* The driver that offered modes last (dev/pciuser.c): none yet, still running (whether or not
       its modes are still offered), or ended (screen_modes_gone). */
    enum { DRIVER_NONE, DRIVER_RUNNING, DRIVER_ENDED } driver;
    struct n_screen_mode wanted;    /* the last wish (all 0: none) */
    uint64_t wish_serial;           /* its number (struct n_screen_state mode_serial) */
    struct n_screen_mode refused[REFUSED_MAX];
    unsigned nrefused;
    bool refused_last;              /* the mode tried last could not be shown */
    bool trial_on;                  /* trial_from comes back at trial_until (clock ns) unless it is kept */
    uint64_t trial_until;
    struct n_screen_mode trial_from;
};

static struct mutex lock;
static struct choice **choices;     /* by screen number, made at the first offer or question */
static size_t nchoices, choices_room;
struct kept {
    char monitor[sizeof ((struct n_screen_modes *)0)->monitor];
    struct n_screen_mode mode;
};
static struct kept *kept;            /* the user's choice for each monitor */
static size_t nkept, kept_room;
static uint64_t trial_changes;       /* atomic: moves on under lock with each trial's change, before trial_wq's wake */
static struct wait_queue trial_wq;
static bool watch_started;

/* Screen id's choice, made when it is not there yet. lock held; NULL without memory. */
static struct choice *choice_of(unsigned id) {
    while (nchoices <= id) {
        struct choice *c = kzalloc(sizeof *c);
        struct choice **at = c ? kappend(&choices, &nchoices, &choices_room, sizeof *at) : NULL;
        if (!at) {
            kfree(c);
            return NULL;
        }
        *at = c;
    }
    return choices[id];
}

static bool same(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz;
}

static const struct n_screen_mode *offered_mode(struct choice *c, const struct n_screen_mode *m) {
    for (uint32_t i = 0; i < c->offered.count; i++)
        if (same(&c->offered.modes[i], m)) return &c->offered.modes[i];
    return NULL;
}

static bool is_refused(struct choice *c, const struct n_screen_mode *m) {
    for (unsigned i = 0; i < c->nrefused; i++)
        if (same(&c->refused[i], m)) return true;
    return false;
}

static void refuse(struct choice *c, const struct n_screen_mode *m) {
    if (!is_refused(c, m) && c->nrefused < REFUSED_MAX) c->refused[c->nrefused++] = *m;
}

static void unrefuse(struct choice *c, const struct n_screen_mode *m) {
    for (unsigned i = 0; i < c->nrefused; i++)
        if (same(&c->refused[i], m)) {
            c->refused[i] = c->refused[--c->nrefused];
            return;
        }
}

static const struct n_screen_mode *kept_for(const char *monitor) {
    for (size_t i = 0; i < nkept; i++)
        if (!strcmp(kept[i].monitor, monitor)) return &kept[i].mode;
    return NULL;
}

static bool keep_for(const char *monitor, const struct n_screen_mode *m) {
    size_t i = 0;
    while (i < nkept && strcmp(kept[i].monitor, monitor)) i++;
    if (i == nkept) {
        struct kept *k = kappend(&kept, &nkept, &kept_room, sizeof *k);
        if (!k) return false;
        memset(k, 0, sizeof *k);
        strlcpy(k->monitor, monitor, sizeof k->monitor);
    }
    kept[i].mode = (struct n_screen_mode){m->width, m->height, m->refresh_mhz, 0};
    return true;
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* The offered, not refused mode of width x height whose refresh is nearest mhz (the higher of two
   as near); any size when width is 0: the largest. NULL when there is none. */
static const struct n_screen_mode *nearest(struct choice *c, uint32_t width, uint32_t height, uint32_t mhz) {
    const struct n_screen_mode *best = NULL;
    for (uint32_t i = 0; i < c->offered.count; i++) {
        const struct n_screen_mode *m = &c->offered.modes[i];
        if ((width && (m->width != width || m->height != height)) || is_refused(c, m)) continue;
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

/* The mode screen id shows when nothing is on trial: the user's choice for the monitor, else the
   monitor's preferred mode, else its preferred size at another refresh, else its largest size
   at the refresh nearest the preferred one (the highest without a preferred mode); screen 0 without the desktop (the text console keeps the firmware's size) the
   firmware's size first. All 0 when every mode was refused. */
static struct n_screen_mode pick(unsigned id, struct choice *c) {
    const struct n_screen_mode *m = kept_for(c->offered.monitor), *preferred = NULL;
    if (m) m = offered_mode(c, m);
    if (m && !is_refused(c, m)) return *m;
    for (uint32_t i = 0; i < c->offered.count && !preferred; i++)
        if (c->offered.modes[i].flags & N_SCREEN_MODE_PREFERRED) preferred = &c->offered.modes[i];
    uint32_t mhz = preferred ? preferred->refresh_mhz : UINT32_MAX; /* without one: the highest */
    m = NULL;
    if (id == 0 && !wm_running()) m = nearest(c, fb.width, fb.height, mhz);
    if (!m && preferred) m = nearest(c, preferred->width, preferred->height, mhz);
    if (!m) m = nearest(c, 0, 0, mhz);
    if (m) return *m;
    return (struct n_screen_mode){0};
}

static void mode_text(const struct n_screen_mode *m, char *out, size_t cap) {
    if (!m->width) ksnprintf(out, cap, "no mode");
    else ksnprintf(out, cap, "%ux%u at %u.%03u Hz", m->width, m->height, m->refresh_mhz / 1000, m->refresh_mhz % 1000);
}

/* A new wish, numbered anew even for the same mode again (the driver acts on every number);
   the driver hears of it once lock is dropped (publish). */
static void wish(unsigned id, struct choice *c, const struct n_screen_mode *m, const char *why) {
    c->wanted = m->width ? (struct n_screen_mode){m->width, m->height, m->refresh_mhz, m->flags} : (struct n_screen_mode){0};
    c->wish_serial++;
    char text[48];
    mode_text(&c->wanted, text, sizeof text);
    kprintf("screen %u: %s: %s\n", id, why, text);
}

/* Screen id's wish as it is now to the driver's state, and what changed to the watches; outside
   lock (the WM's comes after it). */
static void publish(unsigned id) {
    mutex_lock(&lock);
    struct choice *c = id < nchoices ? choices[id] : NULL;
    struct n_screen_mode m = c ? c->wanted : (struct n_screen_mode){0};
    uint64_t serial = c ? c->wish_serial : 0;
    mutex_unlock(&lock);
    if (c) wm_screen_mode(id, &m, serial);
    wm_screen_info_changed();
}

/* lock held: c's trial off, or on until until. */
static void trial_set(struct choice *c, bool on, uint64_t until) {
    c->trial_on = on;
    c->trial_until = until;
    __atomic_add_fetch(&trial_changes, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&trial_wq);
}

static void trial_watch(void *arg) {
    (void)arg;
    for (;;) {
        mutex_lock(&lock);
        uint64_t now = clock_now_ns(), next = CLOCK_NEVER;
        size_t n = nchoices;
        bool *back = kzalloc(n ? n : 1);
        for (size_t id = 0; id < n; id++) {
            struct choice *c = choices[id];
            if (!c->trial_on) continue;
            if (now < c->trial_until) {
                next = MIN(next, c->trial_until);
                continue;
            }
            if (!back) { /* no memory to say whom: look again soon */
                next = MIN(next, clock_after(NS_PER_MS * 100));
                continue;
            }
            trial_set(c, false, 0);
            wish((unsigned)id, c, &c->trial_from, "the mode on trial was not kept; back to");
            back[id] = true;
        }
        uint64_t seen = __atomic_load_n(&trial_changes, __ATOMIC_SEQ_CST);
        mutex_unlock(&lock);
        for (size_t id = 0; back && id < n; id++)
            if (back[id]) publish((unsigned)id);
        kfree(back);
        wq_wait_unless(&trial_wq, &trial_changes, seen, next);
    }
}

/* The driver's refresh of the mode it shows on the screen, when it shows the screen's size. */
static void refresh_from(unsigned id, const struct n_screen_modes *m) {
    struct screen *s = screen_get(id);
    struct screen_mode_desc d;
    if (!s || !m->shown.width || !screen_mode(s, &d) || d.width != m->shown.width || d.height != m->shown.height) return;
    screen_set_refresh(s, m->shown.refresh_mhz, m->shown.flags & N_SCREEN_MODE_VRR ? m->vrr_min_mhz : 0);
}

void screen_modes_offer(unsigned id, const struct n_screen_modes *m) {
    mutex_lock(&lock);
    struct choice *c = choice_of(id);
    if (!c) {
        mutex_unlock(&lock);
        return;
    }
    if (!m->count) {
        if (c->offered.count) kprintf("screen %u: the display driver drives no monitor there any more\n", id);
        c->offered.count = 0;
        if (c->trial_on) trial_set(c, false, 0);
        c->nrefused = 0;
        if (c->wanted.width) wish(id, c, &(struct n_screen_mode){0}, "no display driver offers modes");
        mutex_unlock(&lock);
        publish(id);
        return;
    }
    c->driver = DRIVER_RUNNING;
    bool new_monitor = !c->offered.count || strncmp(c->offered.monitor, m->monitor, sizeof c->offered.monitor);
    c->offered = *m;
    c->offered.monitor[sizeof c->offered.monitor - 1] = 0;
    c->offered.name[sizeof c->offered.name - 1] = 0;
    if (c->offered.count > N_SCREEN_MODES_MAX) c->offered.count = N_SCREEN_MODES_MAX;
    if (m->done == 0) {
        /* Offered anew (a monitor connected, the driver started): it acts on the next wish. */
        if (new_monitor) {
            kprintf("screen %u: monitor %s%s%s, %u modes\n", id, c->offered.monitor, c->offered.name[0] ? ", " : "",
                    c->offered.name, c->offered.count);
            c->nrefused = 0;
            if (c->trial_on) trial_set(c, false, 0);
            c->refused_last = false;
        }
        bool keep = !new_monitor && c->wanted.width && offered_mode(c, &c->wanted) && !is_refused(c, &c->wanted);
        if (keep) wish(id, c, &c->wanted, "the monitor is offered again; it shows");
        else {
            struct n_screen_mode p = pick(id, c);
            wish(id, c, &p, "the monitor shows");
        }
    } else if (m->done == c->wish_serial && c->wanted.width && !same(&m->shown, &c->wanted)) {
        /* The driver could not show the wish: the one before a trial comes back, else the next best. */
        char text[48];
        mode_text(&c->wanted, text, sizeof text);
        kprintf("screen %u: the display driver could not show %s\n", id, text);
        refuse(c, &c->wanted);
        c->refused_last = true;
        if (c->trial_on) {
            trial_set(c, false, 0);
            wish(id, c, &c->trial_from, "back to");
        } else {
            struct n_screen_mode p = pick(id, c);
            wish(id, c, p.width ? &p : &m->shown, "instead it shows");
        }
    }
    /* A result of the wish (or of an older one: the driver acts on the newer one next) changes nothing. */
    mutex_unlock(&lock);
    refresh_from(id, m);
    publish(id);
}

void screen_modes_gone(unsigned id) {
    static const struct n_screen_modes none;
    mutex_lock(&lock);
    struct choice *c = choice_of(id);
    if (c) c->driver = DRIVER_ENDED;
    mutex_unlock(&lock);
    screen_modes_offer(id, &none);
}

void screen_modes_shown(unsigned id, struct screen_mode_desc *desc) {
    mutex_lock(&lock);
    struct choice *c = id < nchoices ? choices[id] : NULL;
    if (c && c->offered.count) {
        memcpy(desc->monitor, c->offered.monitor, MIN(sizeof desc->monitor, sizeof c->offered.monitor));
        desc->monitor[sizeof desc->monitor - 1] = 0;
        memcpy(desc->name, c->offered.name, MIN(sizeof desc->name, sizeof c->offered.name));
        desc->name[sizeof desc->name - 1] = 0;
        /* the mode wished at that size: the driver shows the wish (its refresh), or says it shows another */
        const struct n_screen_mode *m = NULL;
        if (c->offered.shown.width == desc->width && c->offered.shown.height == desc->height) m = &c->offered.shown;
        else if (c->wanted.width == desc->width && c->wanted.height == desc->height) m = &c->wanted;
        if (m) {
            const struct n_screen_mode *o = offered_mode(c, m);
            desc->refresh_mhz = m->refresh_mhz;
            desc->vrr_min_mhz = o && (o->flags & N_SCREEN_MODE_VRR) ? c->offered.vrr_min_mhz : 0;
        }
    }
    mutex_unlock(&lock);
}

/* ---- the modes the user kept ---- */

/* The user's choice of mode for a monitor, applied: shown on each screen that monitor is
   offered on and offers it (now, if it is). */
static void choose(const char *monitor, const struct n_screen_mode *mode) {
    char name[sizeof kept[0].monitor] = {0};
    strlcpy(name, monitor, sizeof name);
    mutex_lock(&lock);
    const struct n_screen_mode *before = kept_for(name);
    bool differs = !before || !same(before, mode);
    keep_for(name, mode);
    size_t n = nchoices;
    bool *wished = kzalloc(n ? n : 1);
    for (size_t id = 0; wished && id < n; id++) {
        struct choice *c = choices[id];
        const struct n_screen_mode *m = c->offered.count && !strcmp(c->offered.monitor, name) ? offered_mode(c, mode) : NULL;
        if (m && !c->trial_on && !same(m, &c->wanted)) {
            unrefuse(c, m);
            wish((unsigned)id, c, m, "the user's choice for this monitor");
            wished[id] = true;
        }
    }
    mutex_unlock(&lock);
    bool any = false;
    for (size_t id = 0; wished && id < n; id++)
        if (wished[id]) {
            publish((unsigned)id);
            any = true;
        }
    kfree(wished);
    if (!any && differs) wm_screen_info_changed(); /* chosen */
    settings_changed(N_DOMAIN_DISPLAY);
}

int screen_modes_setting_put(uint32_t key, const void *value) {
    const struct n_setting_monitor_mode *m = value;
    if (strlen(m->monitor) >= sizeof kept[0].monitor) return -N_ERR_INVALID;
    struct n_screen_mode mode = {m->width, m->height, m->refresh_mhz, 0};
    choose(m->monitor, &mode);
    return 0;
}

int64_t screen_modes_setting_get(uint32_t key, void *out, size_t cap) {
    struct n_setting_monitor_mode *list = out;
    mutex_lock(&lock);
    size_t n = nkept, fits = cap / sizeof *list;
    for (size_t i = 0; i < n && i < fits; i++) {
        memset(&list[i], 0, sizeof list[i]);
        strlcpy(list[i].monitor, kept[i].monitor, sizeof list[i].monitor);
        list[i].width = kept[i].mode.width;
        list[i].height = kept[i].mode.height;
        list[i].refresh_mhz = kept[i].mode.refresh_mhz;
    }
    mutex_unlock(&lock);
    return (int64_t)(n * sizeof *list);
}

_Static_assert(offsetof(struct screen_modes, chosen) == SCREEN_MODES_FIRST_BYTES, "the first struct screen_modes");

/* What the Settings app is shown of a screen without a driver's modes, or while the firmware's
   mode is shown: the screen's monitor and mode as its buffers have it (the refresh its driver or
   the firmware's EDID says, 0 when not known). */
static void screen_part(struct n_screen_modes *m, struct screen *s, bool boot) {
    struct screen_mode_desc d;
    bool on = s && screen_mode(s, &d);
    if (!m->count) {
        memset(m, 0, sizeof *m);
        if (on) {
            memcpy(m->monitor, d.monitor, MIN(sizeof m->monitor, sizeof d.monitor));
            memcpy(m->name, d.name, MIN(sizeof m->name, sizeof d.name));
        }
    }
    if (!boot && m->count) return;
    m->shown = on ? (struct n_screen_mode){d.width, d.height, d.refresh_mhz, 0} : (struct n_screen_mode){0};
}

int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op) {
    unsigned id = (unsigned)(op >> 8);
    op &= 0xff;
    struct screen *s = screen_get(id);
    if (!s) return -N_ERR_NOT_FOUND;
    if (op == SCREEN_MODES_GET) {
        /* the struct of the caller's build: with chosen or without */
        if (size != sizeof(struct screen_modes) && size != SCREEN_MODES_FIRST_BYTES) return -N_ERR_INVALID;
        struct screen_modes *out = kzalloc(sizeof *out);
        if (!out) return -N_ERR_NO_MEMORY;
        bool remote = id == 0 && wm_remote_viewing(), boot = screen_boot(s);
        struct screen_mode_desc d;
        bool on = screen_mode(s, &d);
        mutex_lock(&lock);
        struct choice *c = choice_of(id);
        if (c) {
            out->offered = c->offered;
            out->wanted = c->wanted;
            uint64_t now = clock_now_ns();
            /* the ABI's ms, rounded up: 0 says no trial */
            out->trial_ms = c->trial_on && c->trial_until > now ? (uint32_t)((c->trial_until - now + NS_PER_MS - 1) / NS_PER_MS) : 0;
            out->refused = c->refused_last;
        }
        out->source = remote             ? SCREEN_SOURCE_REMOTE
                      : on && !boot      ? SCREEN_SOURCE_DRIVER
                      : !c               ? SCREEN_SOURCE_FIRMWARE
                      : c->driver == DRIVER_RUNNING ? SCREEN_SOURCE_HANDED_BACK
                      : c->driver == DRIVER_ENDED   ? SCREEN_SOURCE_DRIVER_STOPPED
                                                    : SCREEN_SOURCE_FIRMWARE;
        out->screens = screen_count();
        mutex_unlock(&lock);
        screen_part(&out->offered, s, boot || !on);
        mutex_lock(&lock);
        const struct n_screen_mode *chosen = out->offered.monitor[0] ? kept_for(out->offered.monitor) : NULL;
        if (chosen) out->chosen = *chosen;
        mutex_unlock(&lock);
        int64_t r = copy_to_user(user, out, size);
        kfree(out);
        return r;
    }
    if (op != SCREEN_MODE_SET || size != sizeof(struct screen_mode_set)) return -N_ERR_INVALID;
    struct screen_mode_set set;
    if (copy_from_user(&set, user, sizeof set)) return -N_ERR_BAD_ADDRESS;
    set.monitor[sizeof set.monitor - 1] = 0;
    int64_t r = 0;
    bool changed = false, chose = false, ended = false;
    mutex_lock(&lock);
    struct choice *c = choice_of(id);
    if (!c) r = -N_ERR_NO_MEMORY;
    else if (set.how == SCREEN_MODE_TRY) {
        const struct n_screen_mode *m = c->offered.count ? offered_mode(c, &set.mode) : NULL;
        if (!c->offered.count) r = -N_ERR_NOT_FOUND;
        else if (!m) r = -N_ERR_INVALID;
        else {
            if (!c->trial_on) c->trial_from = c->wanted;
            trial_set(c, true, clock_after((uint64_t)SCREEN_MODE_TRIAL_MS * NS_PER_MS));
            c->refused_last = false;
            unrefuse(c, m);
            wish(id, c, m, "on trial");
            changed = true;
            if (!watch_started) {
                watch_started = true;
                kthread_create("screen mode", trial_watch, NULL);
            }
        }
    } else if (set.how == SCREEN_MODE_KEEP || set.how == SCREEN_MODE_BACK) {
        if (!c->offered.count || !c->trial_on) r = -N_ERR_NOT_FOUND;
        else {
            trial_set(c, false, 0);
            ended = true;
            if (set.how == SCREEN_MODE_KEEP) {
                const struct n_screen_mode *before = kept_for(c->offered.monitor);
                chose = !before || !same(before, &c->wanted);
                if (!keep_for(c->offered.monitor, &c->wanted)) r = -N_ERR_NO_MEMORY;
                char text[48];
                mode_text(&c->wanted, text, sizeof text);
                kprintf("screen %u: the user keeps %s\n", id, text);
            } else {
                wish(id, c, &c->trial_from, "the user goes back to");
                changed = true;
            }
        }
    } else r = -N_ERR_INVALID;
    mutex_unlock(&lock);
    if (changed) publish(id);
    if (chose) settings_changed(N_DOMAIN_DISPLAY); /* the user's choice for this monitor */
    if (ended && !changed) wm_screen_info_changed(); /* the trial ended, and chosen */
    return r;
}
