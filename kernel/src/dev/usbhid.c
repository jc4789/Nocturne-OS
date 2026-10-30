/* USB HID keyboards and pointing devices (Device Class Definition for HID 1.11, HID Usage Tables).
 * Report descriptors are parsed so wheels, extra buttons, tablets and non-boot keyboards work;
 * keys are fed to the input layer as scancode set 1 like the PS/2 and Hyper-V keyboards. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/input.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "sys/clock.h"

#define HID_GET_REPORT_DESC 0x06
#define HID_SET_REPORT 0x09
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B
#define REPEAT_DELAY (500 * NS_PER_MS)
#define REPEAT_RATE (33 * NS_PER_MS)

struct value { /* one parsed input value */
    uint16_t bit, size;
    int32_t min, max;
    uint8_t report;
    bool present, relative;
};

struct keyfield {
    uint8_t report;
    bool variable;
    uint16_t bit, size, count;
    int32_t min;
    uint16_t usage_min, usage_max;
};

struct hid {
    struct hid *next;
    struct usb_device *dev;
    uint8_t interface;
    bool ids;             /* reports start with a report ID byte */
    bool keyboard, mouse;
    /* keyboard */
    struct keyfield keys[16];
    int nkeys;
    uint32_t down[8];     /* usages currently pressed */
    uint16_t repeat_usage;
    uint64_t repeat_at;
    bool leds;            /* has a boot LED output report */
    /* pointer */
    struct value x, y, wheel, button[8];
    uint8_t buttons;
};

/* Everything in this file runs on the usb thread (dev/usb.h): the list, the lock keys' state and
 * each device's state are its alone. The input layer takes its own lock. */
static struct hid *hids;
static bool caps_lock, leds_dirty;

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

static void send_usage(uint16_t usage, bool pressed) {
    uint16_t sc = usage < 256 ? usage_set1[usage] : 0;
    if (!sc) return;
    input_key_set1((uint8_t)((sc & 0x7F) | (pressed ? 0 : 0x80)), sc >> 8); /* one key, its 0xE0 with it */
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

static void key_report(struct hid *h, uint8_t report, const uint8_t *data, unsigned len) {
    uint32_t now_down[8], covered[8] = {0};
    memcpy(now_down, h->down, sizeof now_down);
    bool any = false;
    for (int i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report) continue;
        any = true;
        for (unsigned u = k->usage_min; u <= k->usage_max && u < 256; u++) covered[u >> 5] |= 1u << (u & 31);
    }
    if (!any) return;
    for (int w = 0; w < 8; w++) now_down[w] &= ~covered[w];
    uint32_t bitmap[8] = {0}; /* usages the variable (bitmap) fields of this report decide */
    for (int i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report || !k->variable) continue;
        for (unsigned e = 0; e < k->count; e++) {
            unsigned u = k->usage_min + e;
            if (u >= 256) break;
            bitmap[u >> 5] |= 1u << (u & 31);
            if (bits_at(data, len, k->bit + e * k->size, k->size)) set_down(now_down, u);
        }
    }
    for (int i = 0; i < h->nkeys; i++) {
        struct keyfield *k = &h->keys[i];
        if (k->report != report || k->variable) continue;
        bool phantom = false;
        for (unsigned e = 0; e < k->count && !phantom; e++) {
            int32_t v = signed_at(data, len, k->bit + e * k->size, k->size, k->min < 0);
            phantom = v >= k->min && k->usage_min + (v - k->min) == 1;
        }
        if (phantom) {
            /* ErrorRollOver: too many keys held to tell which (HID Usage Tables 10). The array
             * keeps its previous keys, while the modifier bitmap beside it is still exact. */
            for (unsigned u = k->usage_min; u <= k->usage_max && u < 256; u++)
                if (!is_down(bitmap, u) && is_down(h->down, u)) set_down(now_down, u);
            continue;
        }
        for (unsigned e = 0; e < k->count; e++) {
            int32_t v = signed_at(data, len, k->bit + e * k->size, k->size, k->min < 0);
            unsigned u = (unsigned)(k->usage_min + (v - k->min));
            if (v >= k->min && u >= 4 && u <= k->usage_max && u < 256) set_down(now_down, u);
        }
    }
    /* a modifier pressed in the same report as a key applies to it, and one released in the same
     * report as a key still applied to it: modifier presses first, then key releases and key
     * presses, then modifier releases */
    uint64_t now = clock_now_ns();
    for (unsigned u = 0xE0; u < 0xE8; u++)
        if (!is_down(h->down, u) && is_down(now_down, u)) send_usage(u, true);
    for (unsigned u = 0; u < 256; u++)
        if (u < 0xE0 || u >= 0xE8)
            if (is_down(h->down, u) && !is_down(now_down, u)) {
                send_usage(u, false);
                if (u == h->repeat_usage) h->repeat_usage = 0;
            }
    for (unsigned u = 0; u < 256; u++)
        if ((u < 0xE0 || u >= 0xE8) && !is_down(h->down, u) && is_down(now_down, u)) {
            send_usage(u, true);
            if (u == 0x39) {
                caps_lock = !caps_lock;
                leds_dirty = true;
            }
            if (u < 0xE0 && u != 0x39 && u != 0x53 && u != 0x47) {
                h->repeat_usage = u;
                h->repeat_at = now + REPEAT_DELAY;
            }
        }
    for (unsigned u = 0xE0; u < 0xE8; u++)
        if (is_down(h->down, u) && !is_down(now_down, u)) send_usage(u, false);
    memcpy(h->down, now_down, sizeof now_down);
}

/* ---- pointer ---- */

static int32_t value_of(const struct value *v, const uint8_t *data, unsigned len) {
    return signed_at(data, len, v->bit, v->size, v->min < 0);
}

static int scale_abs(const struct value *v, int32_t raw) {
    if (v->max <= v->min) return 0;
    if (raw < v->min) raw = v->min;
    if (raw > v->max) raw = v->max;
    return (int)((int64_t)(raw - v->min) * MOUSE_ABS_MAX / (v->max - v->min));
}

/* Buttons, motion and wheel may each come in a report of their own (HID 1.11 5.6): a report
 * updates what it carries, and buttons it does not carry keep their last state. */
static void mouse_report(struct hid *h, uint8_t report, const uint8_t *data, unsigned len) {
    struct mouse_event e = {0};
    bool any = false;
    uint8_t buttons = h->buttons;
    for (int i = 0; i < 8; i++) {
        if (!h->button[i].present || h->button[i].report != report) continue;
        any = true;
        if (bits_at(data, len, h->button[i].bit, 1)) buttons |= 1u << i;
        else buttons &= ~(1u << i);
    }
    e.buttons = buttons & 7;
    if (h->x.present && h->x.report == report) {
        any = true;
        bool has_y = h->y.present && h->y.report == report;
        int32_t x = value_of(&h->x, data, len), y = has_y ? value_of(&h->y, data, len) : 0;
        if (h->x.relative) {
            e.dx = x;
            e.dy = y;
        } else {
            e.absolute = true;
            e.x = scale_abs(&h->x, x);
            e.y = has_y ? scale_abs(&h->y, y) : 0;
        }
    }
    if (h->wheel.present && h->wheel.report == report) {
        any = true;
        e.wheel = -value_of(&h->wheel, data, len);
    }
    if (!any) return;
    h->buttons = buttons;
    input_mouse(&e);
}

static void hid_report(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len) {
    (void)d;
    struct hid *h = ctx;
    uint8_t report = 0;
    if (h->ids) {
        if (!len) return;
        report = *data++;
        len--;
    }
    if (h->keyboard) key_report(h, report, data, len);
    if (h->mouse) mouse_report(h, report, data, len);
}

/* ---- report descriptors ---- */

struct globals { /* the global items Push and Pop save */
    uint16_t page;
    int32_t lmin, lmax;
    uint16_t size, count;
    uint8_t report;
};

struct parse {
    struct globals g, stack[4];
    int sp;
    uint32_t usages[16]; /* full page << 16 | id, resolved when each Usage item is read */
    int nusages;
    uint32_t umin, umax;
    bool have_range;
    uint32_t app; /* usage of the enclosing top-level application collection, 0 outside one */
    int depth, app_depth;
    uint16_t offset[256];
};

static uint32_t usage_at(struct parse *p, unsigned i) {
    if (p->have_range) return p->umin + i <= p->umax ? p->umin + i : p->umax;
    if (p->nusages) return p->usages[i < (unsigned)p->nusages ? i : (unsigned)p->nusages - 1];
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
    if (h->nkeys < 16 && k.usage_max >= k.usage_min) h->keys[h->nkeys++] = k;
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
            k.usage_min = p->have_range ? p->umin & 0xFFFF : first & 0xFFFF;
            k.usage_max = p->have_range ? p->umax & 0xFFFF : (uint16_t)(k.usage_min + (p->g.lmax - p->g.lmin));
            add_keyfield(h, k);
            return;
        }
        /* one bit per key; a usage list need not be contiguous, so each run becomes its own field */
        unsigned n = p->have_range ? p->g.count : (p->g.count < (unsigned)p->nusages ? p->g.count : (unsigned)p->nusages);
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
            case 7: p->g.size = (uint16_t)u; break;
            case 8: p->g.report = (uint8_t)u; h->ids = true; break;
            case 9: p->g.count = (uint16_t)u; break;
            case 10: if (p->sp < 4) p->stack[p->sp++] = p->g; break;
            case 11: if (p->sp > 0) p->g = p->stack[--p->sp]; break;
            }
            if (tag == 2 && p->g.lmin >= 0 && p->g.lmax < p->g.lmin) p->g.lmax = (int32_t)u; /* unsigned maximum */
        } else if (type == 2) { /* local */
            uint32_t full = size == 4 ? u : (uint32_t)p->g.page << 16 | (uint16_t)u;
            if (tag == 0 && p->nusages < 16) p->usages[p->nusages++] = full;
            else if (tag == 1) { p->umin = full; p->have_range = true; }
            else if (tag == 2) { p->umax = full; p->have_range = true; }
        } else if (type == 0) { /* main */
            if (tag == 8) {
                input_item(h, p, u);
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
            } else if (tag == 9 && usage_at(p, 0) >> 16 == 0x08 && keyboard_app(p->app)) {
                h->leds = true;
            }
            p->nusages = 0;
            p->have_range = false;
            p->umin = p->umax = 0;
        }
    }
    kfree(p);
    return true;
}

/* the fixed boot keyboard format: modifier byte, reserved byte, six key codes */
static void boot_keyboard(struct hid *h) {
    h->ids = false;
    h->keyboard = true;
    h->nkeys = 2;
    h->keys[0] = (struct keyfield){.variable = true, .bit = 0, .size = 1, .count = 8, .usage_min = 0xE0, .usage_max = 0xE7};
    h->keys[1] = (struct keyfield){.variable = false, .bit = 16, .size = 8, .count = 6, .usage_min = 0, .usage_max = 255};
    h->leds = true;
}

void usbhid_detach(struct usb_device *d) {
    for (struct hid **pp = &hids; *pp;) {
        struct hid *h = *pp;
        if (h->dev != d) {
            pp = &h->next;
            continue;
        }
        for (unsigned u = 0; u < 256; u++)
            if (is_down(h->down, u)) send_usage(u, false);
        if (h->mouse && h->buttons) {
            struct mouse_event e = {0};
            input_mouse(&e);
        }
        *pp = h->next;
        kfree(h);
    }
}

bool usbhid_claims(const struct usb_interface_desc *intf) { return intf->cls == 3; }

void usbhid_attach(struct usb_device *d, const struct usb_interface_desc *intf, const uint8_t *extra,
                   unsigned extra_len, const struct usb_endpoint_desc *ep) {
    struct hid *h = kzalloc(sizeof *h);
    if (!h) return;
    h->dev = d;
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
        uint8_t *rd = report_len && report_len <= 4096 ? kmalloc(report_len) : NULL;
        int got = rd ? usb_control(d, 0x81, USB_REQ_GET_DESCRIPTOR, 0x2200, h->interface, rd, report_len) : -1;
        if (got > 0) parse_report_descriptor(h, rd, (unsigned)got);
        kfree(rd);
        h->mouse = h->x.present && h->y.present;
        h->keyboard = h->nkeys > 0;
        if (!h->mouse && boot && intf->protocol == 2) { /* boot mouse: buttons, X, Y */
            usb_control(d, 0x21, HID_SET_PROTOCOL, 0, h->interface, NULL, 0);
            h->wheel = (struct value){0};
            memset(h->button, 0, sizeof h->button);
            h->ids = false;
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
        kfree(h);
        return;
    }
    usb_control(d, 0x21, HID_SET_IDLE, 0, h->interface, NULL, 0); /* report changes only */
    h->next = hids;
    hids = h;
    if (h->mouse) input_mouse_attach();
    if (h->keyboard) leds_dirty = true;
    kprintf("usbhid: slot %u interface %u is a %s%s%s%s\n", d->slot, h->interface,
            h->keyboard ? "keyboard" : "", h->keyboard && h->mouse ? " and " : "",
            h->mouse ? (h->x.relative ? "mouse" : "absolute pointer") : "", h->wheel.present ? " with wheel" : "");
    if (!usb_interrupt_start(d, ep, hid_report, h)) kprintf("usbhid: slot %u reports could not start\n", d->slot);
}

uint64_t usbhid_tick(uint64_t now) {
    if (leds_dirty) {
        leds_dirty = false;
        uint8_t leds = caps_lock ? 2 : 0;
        for (struct hid *h = hids; h; h = h->next)
            if (h->keyboard && h->leds && !h->ids)
                usb_control(h->dev, 0x21, HID_SET_REPORT, 0x0200, h->interface, &leds, 1);
    }
    uint64_t next = CLOCK_NEVER;
    for (struct hid *h = hids; h; h = h->next) {
        if (!h->repeat_usage) continue;
        if (now >= h->repeat_at) {
            send_usage(h->repeat_usage, true);
            h->repeat_at = now + REPEAT_RATE;
        }
        next = MIN(next, h->repeat_at);
    }
    return next;
}
