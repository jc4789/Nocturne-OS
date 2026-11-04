/* The keys and pointer moves of every input driver (USB, Hyper-V, a remote viewer), on any CPU.
   The events are HID usages and amounts: which character a key makes is the program's libc's
   (n_key_translate). No lock on an event's way: the modifiers and caps lock are words changed
   atomically, a device's values are atomic words, and the sinks are called by the driver directly,
   so a key waits for nothing here.

   The values (N_DOMAIN_INPUT) are the user's defaults and each device's own choice. The list of
   devices and of choices is under input_lock; a change writes the values in effect into the devices. */
#include "kernel.h"
#include "abi.h"
#include "settings_abi.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/input.h"
#include "dev/pm.h"
#include "sys/clock.h"
#include "mm/heap.h"
#include "sys/settings.h"


static key_sink_t key_sink;     /* atomic */
static mouse_sink_t mouse_sink; /* atomic */
static uint8_t mods;            /* atomic: NMOD_* held now, and NMOD_CAPS */
static uint8_t left_alt_held, right_alt_held; /* atomic */
static bool mouse_ok;           /* atomic: any driver has attached a pointer */

/* What an event's way reads of the values, in one place for the defaults and for a choice. */
struct values {
    int32_t speed, accel, natural, repeat_delay_ms, repeat_interval_ms, layout;
};
static struct spinlock input_lock;
static struct values defaults = {.speed = 0, .accel = 1, .natural = 0, .repeat_delay_ms = N_INPUT_REPEAT_DELAY_DEFAULT,
                                 .repeat_interval_ms = N_INPUT_REPEAT_INTERVAL_DEFAULT, .layout = N_LAYOUT_US};
static int32_t double_click_ms = N_INPUT_DOUBLE_CLICK_DEFAULT; /* atomic */
static struct input_device *devices;
struct choice {
    struct choice *next;
    struct values v;
    char device[]; /* with its end */
};
static struct choice *choices;

static uint64_t last_input; /* atomic */

uint64_t input_last_ns(void) { return __atomic_load_n(&last_input, __ATOMIC_RELAXED); }

static const struct choice *choice_of(const char *id) {
    for (const struct choice *c = choices; c; c = c->next)
        if (!strcmp(c->device, id)) return c;
    return NULL;
}

/* input_lock held. */
static void device_values(struct input_device *d) {
    const struct choice *c = choice_of(d->id);
#define PICK(field) (c && c->v.field != N_INPUT_SAME ? c->v.field : defaults.field)
    int32_t speed = PICK(speed);
    if (speed != __atomic_load_n(&d->speed, __ATOMIC_RELAXED)) d->rest_x = d->rest_y = 0;
    __atomic_store_n(&d->speed, speed, __ATOMIC_RELAXED);
    __atomic_store_n(&d->accel, PICK(accel), __ATOMIC_RELAXED);
    __atomic_store_n(&d->natural, PICK(natural), __ATOMIC_RELAXED);
    __atomic_store_n(&d->repeat_delay_ms, PICK(repeat_delay_ms), __ATOMIC_RELAXED);
    __atomic_store_n(&d->repeat_interval_ms, PICK(repeat_interval_ms), __ATOMIC_RELAXED);
    __atomic_store_n(&d->layout, PICK(layout), __ATOMIC_RELAXED);
#undef PICK
}

void input_device_add(struct input_device *d) {
    uint64_t f = spin_lock(&input_lock);
    device_values(d);
    d->next = devices;
    devices = d;
    spin_unlock(&input_lock, f);
    if (d->kinds & N_INPUT_POINTER) __atomic_store_n(&mouse_ok, true, __ATOMIC_RELAXED);
    settings_changed(N_DOMAIN_INPUT);
}

void input_device_remove(struct input_device *d) {
    uint64_t f = spin_lock(&input_lock);
    for (struct input_device **p = &devices; *p; p = &(*p)->next)
        if (*p == d) {
            *p = d->next;
            break;
        }
    spin_unlock(&input_lock, f);
    settings_changed(N_DOMAIN_INPUT);
}

/* ---- events ---- */

static void emit_key(uint16_t usage, uint32_t text, bool pressed, uint32_t layout, uint64_t at) {
    __atomic_store_n(&last_input, clock_now_ns(), __ATOMIC_RELAXED);
    struct key_event e = {.usage = usage, .mods = __atomic_load_n(&mods, __ATOMIC_ACQUIRE), .pressed = pressed,
                          .text = text, .layout = layout, .time_ns = at};
    key_sink_t sink = __atomic_load_n(&key_sink, __ATOMIC_ACQUIRE);
    if (sink) sink(&e);
}

static void modifier(uint8_t bit, bool pressed) {
    if (pressed) __atomic_fetch_or(&mods, bit, __ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&mods, (uint8_t)~bit, __ATOMIC_ACQ_REL);
}

/* A modifier key held or let go. The event that follows it carries the modifiers as they are after it. */
static void modifiers_change(uint16_t usage, bool pressed) {
    switch (usage) {
    case N_USAGE_LEFT_CTRL: case N_USAGE_RIGHT_CTRL: modifier(NMOD_CTRL, pressed); break;
    case N_USAGE_LEFT_SHIFT: case N_USAGE_RIGHT_SHIFT: modifier(NMOD_SHIFT, pressed); break;
    case N_USAGE_LEFT_GUI: case N_USAGE_RIGHT_GUI: modifier(NMOD_SUPER, pressed); break;
    case N_USAGE_LEFT_ALT:
        __atomic_store_n(&left_alt_held, pressed, __ATOMIC_RELEASE);
        modifier(NMOD_ALT, pressed || __atomic_load_n(&right_alt_held, __ATOMIC_ACQUIRE));
        break;
    case N_USAGE_RIGHT_ALT:
        __atomic_store_n(&right_alt_held, pressed, __ATOMIC_RELEASE);
        modifier(NMOD_RIGHT_ALT, pressed);
        modifier(NMOD_ALT, pressed || __atomic_load_n(&left_alt_held, __ATOMIC_ACQUIRE));
        break;
    case N_USAGE_CAPS_LOCK:
        if (pressed) __atomic_fetch_xor(&mods, (uint8_t)NMOD_CAPS, __ATOMIC_ACQ_REL);
        break;
    default: break;
    }
}

void input_key(struct input_device *d, uint16_t usage, bool pressed, uint64_t at) {
    modifiers_change(usage, pressed);
    emit_key(usage, 0, pressed, (uint32_t)__atomic_load_n(&d->layout, __ATOMIC_RELAXED), at);
}

void input_text(struct input_device *d, uint32_t c, bool pressed, uint64_t at) {
    if (c == '\r') c = '\n';
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || !c) return;
    emit_key(N_USAGE_NONE, c, pressed, (uint32_t)__atomic_load_n(&d->layout, __ATOMIC_RELAXED), at);
}

bool input_caps_on(void) { return __atomic_load_n(&mods, __ATOMIC_ACQUIRE) & NMOD_CAPS; }

uint64_t input_repeat_delay_ns(const struct input_device *d) {
    return (uint64_t)__atomic_load_n(&d->repeat_delay_ms, __ATOMIC_RELAXED) * NS_PER_MS;
}
uint64_t input_repeat_interval_ns(const struct input_device *d) {
    return (uint64_t)__atomic_load_n(&d->repeat_interval_ms, __ATOMIC_RELAXED) * NS_PER_MS;
}
uint64_t input_double_click_ns(void) {
    return (uint64_t)__atomic_load_n(&double_click_ms, __ATOMIC_RELAXED) * NS_PER_MS;
}

/* One axis of a relative move: doubled above 5 counts when accelerated, then the speed in quarters
   (x0.5, x0.75, x1, x1.5, x2); a slow move keeps its remainder. */
static int scale(int amount, bool accel, int quarters, int *rest) {
    if (accel && ABS(amount) > 5) amount *= 2;
    if (quarters == 4) return amount;
    *rest += amount * quarters;
    int out = *rest / 4;
    *rest -= out * 4;
    return out;
}

void input_mouse(struct input_device *d, const struct mouse_event *e) {
    static const int quarters[5] = {2, 3, 4, 6, 8};
    __atomic_store_n(&last_input, clock_now_ns(), __ATOMIC_RELAXED);
    struct mouse_event out = *e;
    if (!e->absolute) {
        bool accel = __atomic_load_n(&d->accel, __ATOMIC_RELAXED);
        int q = quarters[__atomic_load_n(&d->speed, __ATOMIC_RELAXED) + 2];
        out.dx = scale(e->dx, accel, q, &d->rest_x);
        out.dy = scale(e->dy, accel, q, &d->rest_y);
    }
    if (__atomic_load_n(&d->natural, __ATOMIC_RELAXED)) out.wheel = -e->wheel;
    mouse_sink_t sink = __atomic_load_n(&mouse_sink, __ATOMIC_ACQUIRE);
    if (sink) sink(&out);
}

void input_resumed(void) {
    __atomic_fetch_and(&mods, (uint8_t)NMOD_CAPS, __ATOMIC_ACQ_REL);
    __atomic_store_n(&left_alt_held, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&right_alt_held, 0, __ATOMIC_RELEASE);
}

void input_set_sinks(key_sink_t k, mouse_sink_t m) {
    __atomic_store_n(&key_sink, k, __ATOMIC_RELEASE);
    __atomic_store_n(&mouse_sink, m, __ATOMIC_RELEASE);
}

bool input_mouse_present(void) { return __atomic_load_n(&mouse_ok, __ATOMIC_RELAXED); }

/* ---- the values (N_DOMAIN_INPUT) ---- */

static bool all_same(const struct values *v) {
    return v->speed == N_INPUT_SAME && v->accel == N_INPUT_SAME && v->natural == N_INPUT_SAME &&
           v->repeat_delay_ms == N_INPUT_SAME && v->repeat_interval_ms == N_INPUT_SAME && v->layout == N_INPUT_SAME;
}

static bool in_range(int32_t v, int32_t low, int32_t high) { return v == N_INPUT_SAME || (v >= low && v <= high); }

int input_device_record_put(uint32_t key, const void *record, size_t length) {
    (void)key;
    struct n_setting_input_device in;
    if (length < sizeof in) return -N_ERR_INVALID;
    memcpy(&in, record, sizeof in);
    if (in.size != length || !in.id_length || in.id_length > length - sizeof in) return -N_ERR_INVALID;
    const char *id = (const char *)record + sizeof in;
    if (strnlen(id, in.id_length) != in.id_length) return -N_ERR_INVALID;
    struct values v = {in.pointer_speed, in.pointer_accel, in.scroll_natural, in.repeat_delay_ms, in.repeat_interval_ms, in.layout};
    if (!in_range(v.speed, -2, 2) || !in_range(v.accel, 0, 1) || !in_range(v.natural, 0, 1) ||
        !in_range(v.repeat_delay_ms, 1, INT32_MAX) || !in_range(v.repeat_interval_ms, 1, INT32_MAX) ||
        !in_range(v.layout, 0, INT32_MAX))
        return -N_ERR_INVALID;
    struct choice *fresh = NULL;
    if (!all_same(&v)) {
        fresh = kmalloc(sizeof *fresh + in.id_length + 1);
        if (!fresh) return -N_ERR_NO_MEMORY;
        fresh->v = v;
        memcpy(fresh->device, id, in.id_length);
        fresh->device[in.id_length] = 0;
    }
    struct choice *gone = NULL;
    uint64_t f = spin_lock(&input_lock);
    for (struct choice **p = &choices; *p; p = &(*p)->next)
        if (strlen((*p)->device) == in.id_length && !memcmp((*p)->device, id, in.id_length)) {
            gone = *p;
            *p = gone->next;
            break;
        }
    if (fresh) {
        fresh->next = choices;
        choices = fresh;
    }
    for (struct input_device *d = devices; d; d = d->next) device_values(d);
    spin_unlock(&input_lock, f);
    kfree(gone);
    settings_changed(N_DOMAIN_INPUT);
    return 0;
}

int input_setting_put(uint32_t key, const void *value) {
    int32_t v;
    memcpy(&v, value, sizeof v);
    bool changed = false;
    uint64_t f = spin_lock(&input_lock);
#define SET(field) do { changed = defaults.field != v; defaults.field = v; } while (0)
    switch (key) {
    case N_INPUT_POINTER_SPEED: SET(speed); break;
    case N_INPUT_POINTER_ACCEL: SET(accel); break;
    case N_INPUT_SCROLL_NATURAL: SET(natural); break;
    case N_INPUT_REPEAT_DELAY_MS: SET(repeat_delay_ms); break;
    case N_INPUT_REPEAT_INTERVAL_MS: SET(repeat_interval_ms); break;
    case N_INPUT_LAYOUT: SET(layout); break;
    case N_INPUT_DOUBLE_CLICK_MS:
        changed = __atomic_exchange_n(&double_click_ms, v, __ATOMIC_RELAXED) != v;
        break;
    default: break;
    }
#undef SET
    for (struct input_device *d = devices; d; d = d->next) device_values(d);
    spin_unlock(&input_lock, f);
    if (changed) settings_changed(N_DOMAIN_INPUT);
    return 0;
}

/* Writes a record at out + at if it fits in cap; the bytes it takes, with the padding to the next. */
static size_t record_put(void *out, size_t cap, size_t at, const char *id, const char *name, uint32_t state, const struct values *v) {
    size_t idn = strlen(id), namen = strlen(name), size = sizeof(struct n_setting_input_device) + idn + namen;
    size_t next = N_INPUT_RECORD_NEXT(size);
    if (out && at + size <= cap) {
        struct n_setting_input_device r = {
            .size = (uint32_t)size, .id_length = (uint32_t)idn, .name_length = (uint32_t)namen, .state = state,
            .pointer_speed = v->speed, .pointer_accel = v->accel, .scroll_natural = v->natural,
            .repeat_delay_ms = v->repeat_delay_ms, .repeat_interval_ms = v->repeat_interval_ms, .layout = v->layout,
        };
        char *to = (char *)out + at;
        memcpy(to, &r, sizeof r);
        memcpy(to + sizeof r, id, idn);
        memcpy(to + sizeof r + idn, name, namen);
    }
    return next;
}

/* The devices attached and the choices of those that are not; each once. */
static int64_t device_list_get(void *out, size_t cap) {
    static const struct values same = {N_INPUT_SAME, N_INPUT_SAME, N_INPUT_SAME, N_INPUT_SAME, N_INPUT_SAME, N_INPUT_SAME};
    size_t at = 0;
    uint64_t f = spin_lock(&input_lock);
    for (const struct input_device *d = devices; d; d = d->next) {
        const struct choice *c = choice_of(d->id);
        at += record_put(out, cap, at, d->id, d->name ? d->name : "", d->kinds | N_INPUT_ATTACHED, c ? &c->v : &same);
    }
    for (const struct choice *c = choices; c; c = c->next) {
        bool attached = false;
        for (const struct input_device *d = devices; d && !attached; d = d->next) attached = !strcmp(d->id, c->device);
        if (!attached) at += record_put(out, cap, at, c->device, "", 0, &c->v);
    }
    spin_unlock(&input_lock, f);
    return (int64_t)at;
}

int64_t input_setting_get(uint32_t key, void *out, size_t cap) {
    if (key == N_INPUT_DEVICE) return device_list_get(out, cap);
    int32_t v = 0;
    uint64_t f = spin_lock(&input_lock);
    switch (key) {
    case N_INPUT_POINTER_SPEED: v = defaults.speed; break;
    case N_INPUT_POINTER_ACCEL: v = defaults.accel; break;
    case N_INPUT_SCROLL_NATURAL: v = defaults.natural; break;
    case N_INPUT_REPEAT_DELAY_MS: v = defaults.repeat_delay_ms; break;
    case N_INPUT_REPEAT_INTERVAL_MS: v = defaults.repeat_interval_ms; break;
    case N_INPUT_LAYOUT: v = defaults.layout; break;
    case N_INPUT_DOUBLE_CLICK_MS: v = __atomic_load_n(&double_click_ms, __ATOMIC_RELAXED); break;
    default: break;
    }
    spin_unlock(&input_lock, f);
    return setting_int(out, cap, v);
}
