/* USB HID keyboards and pointing devices (Device Class Definition for HID 1.11, HID Usage Tables).
 * Report descriptors are parsed so wheels, extra buttons, tablets and non-boot keyboards work;
 * keys are fed to the input layer as scancode set 1 like the Hyper-V keyboard. A report goes to
 * the input layer from the drain that took its completion (dev/usb.h), with that moment. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/input.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "sys/alarm.h"
#include "sys/clock.h"

#define HID_GET_REPORT_DESC 0x06
#define HID_SET_REPORT 0x09
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B
#define REPEAT_DELAY (500 * NS_PER_MS)
#define REPEAT_RATE (33 * NS_PER_MS)
/* A report's bits at most, its ID byte beside them: Get_Report's wLength has 16 bits (HID 7.2.1). */
#define REPORT_BITS_MOST (0xFFFEull * 8)

struct value { /* one parsed input value */
    uint32_t bit, size;
    int32_t min, max;
    uint8_t report;
    bool present, relative;
};

struct keyfield {
    uint8_t report;
    bool variable;
    uint32_t bit, size, count;
    int32_t min;
    uint16_t usage_min, usage_max;
};

/* One HID interface. What its reports change (down, repeat, buttons) is under lock, between its
   reports and its key repeat alarm; the rest is set at attach and stays. */
struct hid {
    struct hid *next;     /* in hids */
    struct hid *sibling;  /* the device's other HID interfaces (its worker's) */
    struct usb_device *dev;
    struct spinlock lock;
    uint8_t interface;
    bool ids;             /* reports start with a report ID byte */
    bool keyboard, mouse;
    /* keyboard */
    struct keyfield *keys;
    size_t nkeys, keys_room;
    uint32_t down[8];     /* usages currently pressed */
    uint16_t repeat_usage;
    uint64_t repeat_at;
    struct alarm repeat;  /* the held key's next repeat */
    bool leds;            /* has an output report with the Caps Lock LED (HID 7.2.2, HUT 11) */
    uint8_t led_report;   /* its ID, 0 without */
    uint32_t led_bit, led_len; /* the LED's bit in the report's data, and the data's bytes */
    unsigned leds_seen;   /* the leds_gen its lights show (its worker's) */
    unsigned longest;     /* bytes of its longest input report, with the ID; 0: not known */
    uint16_t report_bytes[256]; /* of each input report by its ID, with the ID; 0: not known */
    /* Under lock, what has come of a report that takes more than one packet: a USB transfer holds one
       report, which may take several (HID 8.4). The endpoint's TDs are a packet each, of `packet`
       bytes, so a report that is whole does not wait for the next one's. */
    uint8_t *partial;
    unsigned partial_len, packet;
    /* pointer */
    struct value x, y, wheel, button[8];
    uint8_t buttons;
    int last_x, last_y;   /* an absolute pointer's place, kept through a value out of range (HID 5.10) */
    bool x_known, y_known;
};

/* Every HID interface, under hids_lock: a caps lock press on one keyboard asks every keyboard's
   worker to light it. caps_lock and leds_gen are atomic. */
static struct spinlock hids_lock;
static struct hid *hids;
static uint8_t caps_lock;
static unsigned leds_gen;

/* HID usage (keyboard page) to scancode set 1; 0xE0xx marks the extended prefix */
static const uint16_t usage_set1[256] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    [0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    [0x28] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,
    0x35, 0x3A,
    [0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    [0x46] = 0xE037, 0x46, 0, 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D, 0xE04B, 0xE050, 0xE048,
    [0x53] = 0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52,
    0x53, 0x56, 0xE05D,
    [0x87] = 0x73, 0x70, 0x7D, 0x79, 0x7B,
    [0xE0] = 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C,
};

static void send_usage(uint16_t usage, bool pressed, uint64_t at) {
    uint16_t sc = usage < 256 ? usage_set1[usage] : 0;
    if (!sc) return;
    input_key_set1((uint8_t)((sc & 0x7F) | (pressed ? 0 : 0x80)), sc >> 8, at); /* one key, its 0xE0 with it */
}

static bool is_down(const uint32_t *set, unsigned u) { return set[u >> 5] & (1u << (u & 31)); }
static void set_down(uint32_t *set, unsigned u) { set[u >> 5] |= 1u << (u & 31); }

static uint32_t bits_at(const uint8_t *data, unsigned len, unsigned bit, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size && i < 32; i++) {
        unsigned b = bit + i;
        if ((b >> 3) >= len) break;
        if (data[b >> 3] & (1u << (b & 7))) v |= 1u << i;
    }
    return v;
}

static int32_t signed_at(const uint8_t *data, unsigned len, unsigned bit, unsigned size, bool is_signed) {
    uint32_t v = bits_at(data, len, bit, size);
    if (is_signed && size && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (int32_t)v;
}

/* ---- keyboard ---- */

/* The values of the field that lie in the `len` bytes that came. */
static unsigned values_in(const struct keyfield *k, unsigned len) {
    unsigned bits = len * 8;
    return k->bit >= bits ? 0 : MIN(k->count, (bits - k->bit) / k->size);
}

/* The held key repeats while nothing newer is pressed and it is not let go. */
static void repeat_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct hid *h = (struct hid *)((char *)a - offsetof(struct hid, repeat));
    spin_take(&h->lock);
    uint64_t now = clock_now_ns();
    if (h->repeat_usage && now >= h->repeat_at) {
        send_usage(h->repeat_usage, true, now);
        h->repeat_at += REPEAT_RATE;
        if (h->repeat_at <= now) h->repeat_at = now + REPEAT_RATE;
    }
    if (h->repeat_usage) alarm_set(a, h->repeat_at);
    spin_give(&h->lock);
}

/* h->lock held. True when it pressed caps lock. */
static bool key_report(struct hid *h, uint8_t report, const uint8_t *data, unsigned len, uint64_t now) {
    uint32_t now_down[8], covered[8] = {0};
    memcpy(now_down, h->down, sizeof now_down);
    bool any = false;
    for (size_t i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report) continue;
        any = true;
        for (unsigned u = k->usage_min; u <= k->usage_max && u < 256; u++) covered[u >> 5] |= 1u << (u & 31);
    }
    if (!any) return false;
    for (int w = 0; w < 8; w++) now_down[w] &= ~covered[w];
    uint32_t bitmap[8] = {0}; /* usages the variable (bitmap) fields of this report decide */
    for (size_t i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report || !k->variable) continue;
        for (unsigned e = 0, n = values_in(k, len); e < n; e++) {
            unsigned u = k->usage_min + e;
            if (u >= 256) break;
            bitmap[u >> 5] |= 1u << (u & 31);
            if (bits_at(data, len, k->bit + e * k->size, k->size)) set_down(now_down, u);
        }
    }
    for (size_t i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report || k->variable) continue;
        bool phantom = false;
        unsigned values = values_in(k, len);
        for (unsigned e = 0; e < values && !phantom; e++) {
            int32_t v = signed_at(data, len, k->bit + e * k->size, k->size, k->min < 0);
            /* ErrorRollOver, POSTFail, ErrorUndefined: each says there are no keys to read (HUT 10) */
            unsigned code = (unsigned)(k->usage_min + (v - k->min));
            phantom = v >= k->min && code >= 1 && code <= 3;
        }
        if (phantom) {
            /* The array keeps its previous keys, while the modifier bitmap beside it is still exact. */
            for (unsigned u = k->usage_min; u <= k->usage_max && u < 256; u++)
                if (!is_down(bitmap, u) && is_down(h->down, u)) set_down(now_down, u);
            continue;
        }
        for (unsigned e = 0; e < values; e++) {
            int32_t v = signed_at(data, len, k->bit + e * k->size, k->size, k->min < 0);
            unsigned u = (unsigned)(k->usage_min + (v - k->min));
            if (v >= k->min && u >= 4 && u <= k->usage_max && u < 256) set_down(now_down, u);
        }
    }
    /* a modifier pressed in the same report as a key applies to it, and one released in the same
     * report as a key still applied to it: modifier presses first, then key releases and key
     * presses, then modifier releases */
    bool caps = false;
    for (unsigned u = 0xE0; u < 0xE8; u++)
        if (!is_down(h->down, u) && is_down(now_down, u)) send_usage(u, true, now);
    for (unsigned u = 0; u < 256; u++)
        if (u < 0xE0 || u >= 0xE8)
            if (is_down(h->down, u) && !is_down(now_down, u)) {
                send_usage(u, false, now);
                if (u == h->repeat_usage) h->repeat_usage = 0;
            }
    for (unsigned u = 0; u < 256; u++)
        if ((u < 0xE0 || u >= 0xE8) && !is_down(h->down, u) && is_down(now_down, u)) {
            send_usage(u, true, now);
            if (u == 0x39) caps = true;
            if (u < 0xE0 && u != 0x39 && u != 0x53 && u != 0x47) {
                h->repeat_usage = u;
                h->repeat_at = now + REPEAT_DELAY;
                alarm_set(&h->repeat, h->repeat_at);
            }
        }
    for (unsigned u = 0xE0; u < 0xE8; u++)
        if (is_down(h->down, u) && !is_down(now_down, u)) send_usage(u, false, now);
    memcpy(h->down, now_down, sizeof now_down);
    return caps;
}

/* Caps lock changed: every keyboard's worker lights it (usbhid_work). */
static void caps_toggled(void) {
    __atomic_xor_fetch(&caps_lock, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&leds_gen, 1, __ATOMIC_RELEASE);
    uint64_t f = spin_lock(&hids_lock);
    for (struct hid *h = hids; h; h = h->next)
        if (h->keyboard && h->leds) usb_note_work(h->dev);
    spin_unlock(&hids_lock, f);
}

/* ---- pointer ---- */

static int32_t value_of(const struct value *v, const uint8_t *data, unsigned len) {
    return signed_at(data, len, v->bit, v->size, v->min < 0);
}

/* A value outside the item's logical range says the control has no value now (HID 5.10, 6.2.2.5). */
static bool in_range(const struct value *v, int32_t raw) {
    return v->max <= v->min || (raw >= v->min && raw <= v->max);
}

static int scale_abs(const struct value *v, int32_t raw) {
    if (v->max <= v->min) return 0;
    return (int)((int64_t)(raw - v->min) * MOUSE_ABS_MAX / (v->max - v->min));
}

/* The value is in this report: of its ID, and whole in the `len` bytes that came (a short packet ends
   a report, HID 8.4). */
static bool value_came(const struct value *v, uint8_t report, unsigned len) {
    return v->present && v->report == report && v->bit + v->size <= len * 8;
}

/* Buttons, each axis and wheel may each come in a report of their own (HID 1.11 5.6): a report
 * updates what it carries, and what it does not carry keeps its last state. */
static void mouse_report(struct hid *h, uint8_t report, const uint8_t *data, unsigned len, uint64_t at) {
    struct mouse_event e = {.time_ns = at};
    bool any = false;
    uint8_t buttons = h->buttons;
    for (int i = 0; i < 8; i++) {
        if (!value_came(&h->button[i], report, len)) continue;
        any = true;
        if (bits_at(data, len, h->button[i].bit, 1)) buttons |= 1u << i;
        else buttons &= ~(1u << i);
    }
    e.buttons = buttons & 7;
    bool x_came = value_came(&h->x, report, len), y_came = value_came(&h->y, report, len);
    if (x_came || y_came) {
        any = true;
        int32_t x = x_came ? value_of(&h->x, data, len) : 0, y = y_came ? value_of(&h->y, data, len) : 0;
        bool x_good = x_came && in_range(&h->x, x), y_good = y_came && in_range(&h->y, y);
        if (x_good && h->x.relative) e.dx = x;
        if (y_good && h->y.relative) e.dy = y;
        if (x_good && !h->x.relative) {
            h->last_x = scale_abs(&h->x, x);
            h->x_known = true;
        }
        if (y_good && !h->y.relative) {
            h->last_y = scale_abs(&h->y, y);
            h->y_known = true;
        }
        /* a place only with both axes absolute, each of which has said where */
        if (h->x_known && h->y_known && !h->x.relative && !h->y.relative) {
            e.absolute = true;
            e.x = h->last_x;
            e.y = h->last_y;
        }
    }
    if (value_came(&h->wheel, report, len)) {
        any = true;
        int32_t w = value_of(&h->wheel, data, len);
        e.wheel = in_range(&h->wheel, w) ? -w : 0;
    }
    if (!any) return;
    h->buttons = buttons;
    input_mouse(&e);
}

/* A report whole, h->lock held. True when it pressed caps lock. */
static bool report_whole(struct hid *h, const uint8_t *data, unsigned len, uint64_t at) {
    uint8_t report = 0;
    if (h->ids) {
        if (!len) return false;
        report = *data++;
        len--;
    }
    bool caps = h->keyboard && key_report(h, report, data, len, at);
    if (h->mouse) mouse_report(h, report, data, len, at);
    return caps;
}

/* A packet of the interrupt endpoint, after those kept of its report. A short packet ends the report
   (HID 8.4), and so does its size in the descriptor, of a report that need not end short; what lies
   beyond that size in the packet is padding. A report of no known size is all that came. After a
   recovery dropped the transfers (no data) a report begun is lost. */
static void hid_report(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len, uint64_t at) {
    (void)d;
    struct hid *h = ctx;
    bool caps = false;
    uint64_t f = spin_lock(&h->lock);
    if (data) {
        memcpy(h->partial + h->partial_len, data, len);
        unsigned have = h->partial_len + len;
        unsigned want = have ? h->report_bytes[h->ids ? h->partial[0] : 0] : 0;
        if (have && (!want || have >= want || len < h->packet)) {
            caps = report_whole(h, h->partial, want && want < have ? want : have, at);
            have = 0;
        }
        h->partial_len = have;
    } else {
        h->partial_len = 0;
    }
    spin_unlock(&h->lock, f);
    if (caps) caps_toggled();
}

/* ---- report descriptors ---- */

struct globals { /* the global items Push and Pop save */
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t size, count;
    uint8_t report;
};

/* A Usage item as it was written: 4 bytes name their page, others take the Usage Page in force at the
   main item that uses them (HID 6.2.2.8). */
struct usage {
    uint32_t value;
    bool extended;
};

struct parse {
    struct globals g, *stack;
    size_t sp, stack_room;
    struct usage *usages;
    size_t nusages, usages_room;
    struct usage umin, umax;
    bool have_range;
    bool in_delimiter, delimiter_used; /* of a Delimiter's aliases the first is used (HID 6.2.2.8) */
    uint32_t app; /* usage of the enclosing top-level application collection, 0 outside one */
    int depth, app_depth;
    uint32_t offset[256], out_offset[256]; /* bits of each report so far */
};

static uint32_t usage_full(const struct parse *p, struct usage u) {
    return u.extended ? u.value : (uint32_t)p->g.page << 16 | (uint16_t)u.value;
}

static uint32_t usage_at(struct parse *p, unsigned i) {
    if (p->have_range) {
        uint32_t lo = usage_full(p, p->umin), hi = usage_full(p, p->umax);
        return lo + i <= hi ? lo + i : hi;
    }
    if (p->nusages) return usage_full(p, p->usages[i < p->nusages ? i : p->nusages - 1]);
    return 0;
}

/* Which application collections feed which part of the input layer: keys only from keyboards
 * and keypads, the pointer only from mice, pointers and digitizers (pens, touch screens). A
 * joystick or gamepad also reports X and Y but must not drive the cursor. */
static bool keyboard_app(uint32_t app) { return app == 0x00010006 || app == 0x00010007; }
static bool pointer_app(uint32_t app) {
    return app == 0x00010001 || app == 0x00010002 || (app >= 0x000D0001 && app <= 0x000D0004);
}

static void set_value(struct value *v, struct parse *p, unsigned bit, bool relative) {
    if (v->present) return;
    *v = (struct value){.bit = bit, .size = p->g.size, .min = p->g.lmin, .max = p->g.lmax, .report = p->g.report,
                        .present = true, .relative = relative};
}

static void add_keyfield(struct hid *h, struct keyfield k) {
    if (k.usage_max > 255) k.usage_max = 255;
    if (k.usage_max < k.usage_min) return;
    struct keyfield *at = kappend(&h->keys, &h->nkeys, &h->keys_room, sizeof *h->keys);
    if (at) *at = k;
}

/* An Output item: where its Caps Lock LED is, in the report that has one. */
static void output_item(struct hid *h, struct parse *p, uint32_t flags) {
    unsigned base = p->out_offset[p->g.report];
    p->out_offset[p->g.report] += p->g.size * p->g.count;
    if (flags & 1 || !(flags & 2) || !keyboard_app(p->app) || h->leds) return;
    for (unsigned i = 0; i < p->g.count; i++) {
        if (usage_at(p, i) != 0x00080002) continue;
        h->leds = true;
        h->led_report = p->g.report;
        h->led_bit = base + i * p->g.size;
        return;
    }
}

static void input_item(struct hid *h, struct parse *p, uint32_t flags) {
    unsigned base = p->offset[p->g.report];
    p->offset[p->g.report] += p->g.size * p->g.count;
    if (flags & 1) return; /* constant padding */
    bool variable = flags & 2, relative = flags & 4;
    uint32_t first = usage_at(p, 0);
    if (first >> 16 == 0x07) {
        if (!keyboard_app(p->app)) return;
        struct keyfield k = {.report = p->g.report, .variable = variable, .size = p->g.size, .min = p->g.lmin};
        if (!variable) { /* an array of key codes */
            k.bit = base;
            k.count = p->g.count;
            k.usage_min = first & 0xFFFF;
            k.usage_max = p->have_range ? usage_full(p, p->umax) & 0xFFFF : (uint16_t)(k.usage_min + (p->g.lmax - p->g.lmin));
            add_keyfield(h, k);
            return;
        }
        /* one bit per key; a usage list need not be contiguous, so each run becomes its own field */
        unsigned n = p->have_range ? p->g.count : (p->g.count < p->nusages ? p->g.count : (unsigned)p->nusages);
        for (unsigned i = 0; i < n;) {
            uint32_t u = usage_at(p, i);
            unsigned j = i + 1;
            while (j < n && usage_at(p, j) == u + (j - i)) j++;
            if (u >> 16 == 0x07) {
                k.bit = base + i * p->g.size;
                k.count = j - i;
                k.usage_min = u & 0xFFFF;
                k.usage_max = (uint16_t)((u & 0xFFFF) + (j - i - 1));
                add_keyfield(h, k);
            }
            i = j;
        }
        return;
    }
    if (!variable || !pointer_app(p->app)) return;
    for (unsigned i = 0; i < p->g.count; i++) {
        uint32_t u = usage_at(p, i);
        unsigned bit = base + i * p->g.size;
        if (u == 0x00010030) set_value(&h->x, p, bit, relative);
        else if (u == 0x00010031) set_value(&h->y, p, bit, relative);
        else if (u == 0x00010038) set_value(&h->wheel, p, bit, true);
        else if (u >> 16 == 0x09 && (u & 0xFFFF) >= 1 && (u & 0xFFFF) <= 8 && p->g.size == 1)
            set_value(&h->button[(u & 0xFFFF) - 1], p, bit, false);
    }
}

static bool parse_report_descriptor(struct hid *h, const uint8_t *d, unsigned len) {
    struct parse *p = kzalloc(sizeof *p);
    if (!p) return false;
    p->app_depth = -1;
    bool fits = true;
    for (unsigned i = 0; i < len;) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) { /* long item */
            if (i + 1 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (i + 1 + size > len) break;
        uint32_t u = 0;
        for (unsigned b = 0; b < size; b++) u |= (uint32_t)d[i + 1 + b] << (8 * b);
        int32_t s = size == 1 ? (int8_t)u : size == 2 ? (int16_t)u : (int32_t)u;
        i += 1 + size;
        if (type == 1) { /* global */
            switch (tag) {
            case 0: p->g.page = (uint16_t)u; break;
            case 1: p->g.lmin = s; break;
            case 2: p->g.lmax = s; break;
            case 7: p->g.size = u; break;
            case 8: p->g.report = (uint8_t)u; h->ids = true; break;
            case 9: p->g.count = u; break;
            case 10: {
                struct globals *top = kappend(&p->stack, &p->sp, &p->stack_room, sizeof *p->stack);
                if (top) *top = p->g;
                break;
            }
            case 11: if (p->sp > 0) p->g = p->stack[--p->sp]; break;
            }
            if (tag == 2 && p->g.lmin >= 0 && p->g.lmax < p->g.lmin) p->g.lmax = (int32_t)u; /* unsigned maximum */
        } else if (type == 2) { /* local */
            struct usage us = {u, size == 4};
            bool alias = p->in_delimiter && p->delimiter_used; /* not the first of the set */
            if (tag == 0 && !alias) {
                struct usage *at = kappend(&p->usages, &p->nusages, &p->usages_room, sizeof *p->usages);
                if (at) *at = us;
                p->delimiter_used = p->in_delimiter;
            } else if (tag == 1 && !alias) {
                p->umin = us;
                p->have_range = true;
            } else if (tag == 2 && !alias) {
                p->umax = us;
                p->have_range = true;
                p->delimiter_used = p->in_delimiter;
            } else if (tag == 10) {
                p->in_delimiter = u == 1;
                p->delimiter_used = false;
            }
        } else if (type == 0) { /* main */
            if (tag == 8 || tag == 9) {
                uint32_t have = (tag == 8 ? p->offset : p->out_offset)[p->g.report];
                uint64_t bits = (uint64_t)p->g.size * p->g.count;
                if (have + bits > REPORT_BITS_MOST) {
                    fits = false;
                    break;
                }
                /* an item of no bits has no field to read (6.2.2.7) */
                if (bits && tag == 8) input_item(h, p, u);
                else if (bits) output_item(h, p, u);
            } else if (tag == 10) { /* collection */
                if (u == 1 && p->app_depth < 0) {
                    p->app = usage_at(p, 0);
                    p->app_depth = p->depth;
                }
                p->depth++;
            } else if (tag == 12) { /* end collection */
                if (p->depth > 0) p->depth--;
                if (p->depth == p->app_depth) {
                    p->app = 0;
                    p->app_depth = -1;
                }
            }
            p->nusages = 0;
            p->have_range = false;
            p->umin = p->umax = (struct usage){0};
        }
    }
    for (unsigned r = 0; fits && r < 256; r++) {
        unsigned bytes = (p->offset[r] + 7) / 8 + (h->ids ? 1u : 0);
        if (!p->offset[r]) continue;
        h->report_bytes[r] = (uint16_t)bytes;
        if (bytes > h->longest) h->longest = bytes;
    }
    if (h->leds) h->led_len = (p->out_offset[h->led_report] + 7) / 8;
    kfree(p->stack);
    kfree(p->usages);
    kfree(p);
    return fits;
}

/* What a report descriptor said is forgotten: it was refused, or the boot format takes over. */
static void descriptor_forget(struct hid *h) {
    kfree(h->keys);
    h->keys = NULL;
    h->nkeys = h->keys_room = 0;
    h->x = h->y = h->wheel = (struct value){0};
    memset(h->button, 0, sizeof h->button);
    memset(h->report_bytes, 0, sizeof h->report_bytes);
    h->ids = h->leds = false;
    h->longest = 0;
}

/* the fixed boot keyboard format: modifier byte, reserved byte, six key codes */
static void boot_keyboard(struct hid *h) {
    h->ids = false;
    h->keyboard = true;
    h->nkeys = 0;
    add_keyfield(h, (struct keyfield){.variable = true, .bit = 0, .size = 1, .count = 8, .usage_min = 0xE0, .usage_max = 0xE7});
    add_keyfield(h, (struct keyfield){.variable = false, .bit = 16, .size = 8, .count = 6, .usage_min = 0, .usage_max = 255});
    h->longest = h->report_bytes[0] = 8;
    h->leds = true; /* its LED report (HID B.1): one byte, bit 1 Caps Lock */
    h->led_report = 0;
    h->led_bit = 1;
    h->led_len = 1;
}

/* On the worker, after the device's last report (usb_remove): its held keys and buttons let go. */
void usbhid_detach(struct usb_device *d) {
    while (d->hid) {
        struct hid *h = d->hid;
        d->hid = h->sibling;
        uint64_t f = spin_lock(&hids_lock);
        for (struct hid **pp = &hids; *pp; pp = &(*pp)->next)
            if (*pp == h) {
                *pp = h->next;
                break;
            }
        spin_unlock(&hids_lock, f);
        f = spin_lock(&h->lock);
        h->repeat_usage = 0; /* or a repeat that is firing sets itself again */
        spin_unlock(&h->lock, f);
        alarm_cancel(&h->repeat);
        uint64_t now = clock_now_ns();
        for (unsigned u = 0; u < 256; u++)
            if (is_down(h->down, u)) send_usage(u, false, now);
        if (h->mouse && h->buttons) {
            struct mouse_event e = {.time_ns = now};
            input_mouse(&e);
        }
        kfree(h->keys);
        kfree(h->partial);
        kfree(h);
    }
}

bool usbhid_claims(const struct usb_interface_desc *intf) { return intf->cls == 3; }

void usbhid_attach(struct usb_device *d, const struct usb_interface_desc *intf, const uint8_t *extra,
                   unsigned extra_len, const struct usb_endpoint_desc *ep) {
    struct hid *h = kzalloc(sizeof *h);
    if (!h) return;
    h->dev = d;
    alarm_init(&h->repeat, repeat_fire);
    h->interface = intf->number;
    bool boot = intf->subcls == 1;
    unsigned report_len = 0;
    for (unsigned off = 0; off + 2 <= extra_len && extra[off] >= 2; off += extra[off])
        if (extra[off + 1] == 0x21 && extra[off] >= 9 && off + extra[off] <= extra_len) {
            for (unsigned k = 0; k < extra[off + 5] && off + 9 + 3 * k <= extra[off] + off; k++)
                if (extra[off + 6 + 3 * k] == 0x22) report_len = extra[off + 7 + 3 * k] | extra[off + 8 + 3 * k] << 8;
            break;
        }
    if (boot && intf->protocol == 1) {
        /* keyboards: the boot format is what every firmware relies on */
        usb_control(d, 0x21, HID_SET_PROTOCOL, 0, h->interface, NULL, 0);
        boot_keyboard(h);
    } else {
        uint8_t *rd = report_len ? kmalloc(report_len) : NULL;
        int got = rd ? usb_control(d, 0x81, USB_REQ_GET_DESCRIPTOR, 0x2200, h->interface, rd, report_len) : -1;
        if (got > 0 && !parse_report_descriptor(h, rd, (unsigned)got)) {
            kprintf("usbhid: slot %u interface %u: its report descriptor is refused\n", d->slot, h->interface);
            descriptor_forget(h);
        }
        kfree(rd);
        h->mouse = h->x.present && h->y.present;
        h->keyboard = h->nkeys > 0;
        if (!h->mouse && boot && intf->protocol == 2) { /* boot mouse: buttons, X, Y */
            usb_control(d, 0x21, HID_SET_PROTOCOL, 0, h->interface, NULL, 0);
            descriptor_forget(h);
            h->keyboard = false;
            for (int i = 0; i < 3; i++) h->button[i] = (struct value){.bit = i, .size = 1, .present = true};
            h->x = (struct value){.bit = 8, .size = 8, .min = -127, .max = 127, .present = true, .relative = true};
            h->y = (struct value){.bit = 16, .size = 8, .min = -127, .max = 127, .present = true, .relative = true};
            h->mouse = true;
        } else if (boot) {
            usb_control(d, 0x21, HID_SET_PROTOCOL, 1, h->interface, NULL, 0);
        }
        if (!h->keyboard) h->nkeys = 0;
    }
    if (!h->keyboard && !h->mouse) {
        kfree(h->keys);
        kfree(h);
        return;
    }
    usb_control(d, 0x21, HID_SET_IDLE, 0, h->interface, NULL, 0); /* report changes only */
    h->leds_seen = __atomic_load_n(&leds_gen, __ATOMIC_ACQUIRE) - 1; /* its lights set on the worker's next look */
    h->sibling = d->hid;
    d->hid = h;
    uint64_t f = spin_lock(&hids_lock);
    h->next = hids;
    hids = h;
    spin_unlock(&hids_lock, f);
    if (h->mouse) input_mouse_attach();
    if (h->keyboard) usb_note_work(d);
    kprintf("usbhid: slot %u interface %u is a %s%s%s%s\n", d->slot, h->interface,
            h->keyboard ? "keyboard" : "", h->keyboard && h->mouse ? " and " : "",
            h->mouse ? (h->x.relative ? "mouse" : "absolute pointer") : "", h->wheel.present ? " with wheel" : "");
    h->packet = usb_interrupt_length(d, ep, 1);
    /* room for a report all but whole, and the packet that ends it */
    h->partial = h->packet ? kmalloc(h->longest + h->packet) : NULL;
    if (!h->partial || !usb_interrupt_start(d, ep, 1, hid_report, h))
        kprintf("usbhid: slot %u reports could not start\n", d->slot);
}

/* On the worker: the device's keyboards light caps lock as it is now: Set_Report of its output
   report (HID 7.2.2), led_bit in the data, after the ID if the report has one. */
void usbhid_work(struct usb_device *d) {
    for (struct hid *h = d->hid; h; h = h->sibling) {
        unsigned gen = __atomic_load_n(&leds_gen, __ATOMIC_ACQUIRE);
        if (!h->keyboard || !h->leds || h->leds_seen == gen) continue;
        h->leds_seen = gen;
        unsigned at = h->ids ? 1 : 0, len = at + h->led_len;
        uint8_t *report = kzalloc(len);
        if (!report) continue;
        if (h->ids) report[0] = h->led_report;
        if (__atomic_load_n(&caps_lock, __ATOMIC_ACQUIRE)) report[at + (h->led_bit >> 3)] |= (uint8_t)(1u << (h->led_bit & 7));
        usb_control(h->dev, 0x21, HID_SET_REPORT, (uint16_t)(0x0200 | h->led_report), h->interface, report, (uint16_t)len);
        kfree(report);
    }
}
