/*
 * The monitors' modes (screenmode.h): a mechanism. A display driver knows what its monitor and display
 * engine can show and offers it here. Which of it a screen shows is the display service's to say
 * (user/apps/display.c, N_CAP_DISPLAY): the kernel chooses none and keeps no one's choice. Until a mode
 * is asked for the screen shows what it shows, at boot the firmware's framebuffer. The driver shows each
 * wish at its size and gives the screen buffers of that size (n_screen_give; the desktop follows
 * it), and says what it showed: a wish it could not show is reported as refused, and the service
 * decides what comes next.
 *
 * A driver may give screen 0 back while it runs (a mode its display did not show): the firmware's
 * mode is shown again, and while its modes stay offered a mode applied asks it to show that one
 * again. What decides a screen's size, as struct screen_modes source reports it, tells that apart
 * from a driver that ended.
 *
 * Each screen has its own state (struct choice): the modes offered, the wish and the last wish refused.
 * Every change of what SYS_SCREEN_INFO reports of the modes tells its watches (wm_screen_info_changed).
 *
 * Under lock (a mutex, any CPU; the WM's lock and a screen's use come after it): everything below.
 */
#include "gui/screenmode.h"
#include "gui/wm.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "sys/wait.h"
#include "sys/syscall.h"
#include "mm/heap.h"
#include "display_abi.h"

struct choice {
    struct n_screen_modes offered;  /* count 0: no driver offers modes */
    /* The driver that offered modes last (dev/screen_driver.c): none yet, still running (whether or not
       its modes are still offered), or ended (screen_modes_gone). */
    enum { DRIVER_NONE, DRIVER_RUNNING, DRIVER_ENDED } driver;
    uint64_t offer_serial;          /* moves with each offer that waits for a wish */
    struct n_screen_mode wanted;    /* the last wish (all 0: none) */
    uint64_t wish_serial;           /* its number (struct n_screen_state mode_serial) */
    uint64_t refused_serial;        /* the last wish the driver could not show; 0: none since the monitor came */
};

static struct mutex lock;
static struct choice **choices;     /* by screen number, made at the first offer or question */
static size_t nchoices, choices_room;

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

/* The same mode, format 0 being any. */
static bool same(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz &&
           (a->format == b->format || !a->format || !b->format);
}

static const struct n_screen_mode *offered_mode(struct choice *c, const struct n_screen_mode *m) {
    for (uint32_t i = 0; i < c->offered.count; i++)
        if (same(&c->offered.modes[i], m)) return &c->offered.modes[i];
    return NULL;
}

static void mode_text(const struct n_screen_mode *m, char *out, size_t cap) {
    if (!m->width) ksnprintf(out, cap, "no mode");
    else ksnprintf(out, cap, "%ux%u at %u.%03u Hz", m->width, m->height, m->refresh_mhz / 1000, m->refresh_mhz % 1000);
}

/* A new wish, numbered anew even for the same mode again (the driver acts on every number);
   the driver hears of it once lock is dropped (publish). */
static void wish(unsigned id, struct choice *c, const struct n_screen_mode *m, const char *why) {
    c->wanted = m->width ? *m : (struct n_screen_mode){0};
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
        c->offer_serial++;
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
        c->offer_serial++;
        if (new_monitor) {
            kprintf("screen %u: monitor %s%s%s, %u modes\n", id, c->offered.monitor, c->offered.name[0] ? ", " : "",
                    c->offered.name, c->offered.count);
            c->refused_serial = 0;
        }
    } else if (m->done == c->wish_serial && c->wanted.width && !same(&m->shown, &c->wanted)) {
        char text[48];
        mode_text(&c->wanted, text, sizeof text);
        kprintf("screen %u: the display driver could not show %s\n", id, text);
        c->refused_serial = c->wish_serial;
    }
    /* A result of an older wish (the driver acts on the newer one next) changes nothing. */
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

int64_t screen_modes_apply(unsigned id, const struct n_screen_mode *m) {
    if (!screen_get(id)) return -N_ERR_NOT_FOUND;
    int64_t r = 0;
    mutex_lock(&lock);
    struct choice *c = choice_of(id);
    const struct n_screen_mode *o = c && m->width ? offered_mode(c, m) : NULL;
    if (!c) r = -N_ERR_NO_MEMORY;
    else if (!c->offered.count) r = -N_ERR_NOT_FOUND;
    else if (m->width && !o) r = -N_ERR_INVALID;
    else wish(id, c, m->width ? o : m, "the display service asks for");
    mutex_unlock(&lock);
    if (!r) publish(id);
    return r;
}

/* What a screen without a driver's modes reports, or one showing the firmware's mode: its monitor and
   mode as its buffers have them (the refresh its driver or the firmware's EDID says, 0 when not known). */
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
    m->shown = (struct n_screen_mode){0};
    if (on) {
        m->shown.width = d.width;
        m->shown.height = d.height;
        m->shown.refresh_mhz = d.refresh_mhz;
        if (boot) m->shown.bits_per_channel = fb.bpc;
    }
}

int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op) {
    unsigned id = (unsigned)(op >> 8);
    struct screen *s = screen_get(id);
    if (!s) return -N_ERR_NOT_FOUND;
    if (size != sizeof(struct screen_modes)) return -N_ERR_INVALID;
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
        out->refused_serial = c->refused_serial;
        out->offer_serial = c->offer_serial;
        out->wish_serial = c->wish_serial;
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
    wm_screen_report(id, out);
    int64_t r = copy_to_user(user, out, size);
    kfree(out);
    return r;
}
