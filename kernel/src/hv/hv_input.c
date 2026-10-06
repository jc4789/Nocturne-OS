/* Hyper-V synthetic keyboard and mouse (VMBus). The keyboard sends scancode set 1, which goes
   through the same decoder as PS/2. The mouse is a HID device with absolute coordinates; we read
   its report descriptor to find the buttons, X, Y and wheel. */
#include "kernel.h"
#include "dev/input.h"
#include "hv/vmbus.h"

static const struct guid kbd_guid =
    GUID_INIT(0xf912ad6d, 0x2b17, 0x48ea, 0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84);
static const struct guid mouse_guid =
    GUID_INIT(0xcfa8b69e, 0x5b4a, 0x4cc0, 0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a);

#define RING_BYTES (40 * 1024)

/* ---- keyboard ---- */

enum { KBD_PROTOCOL_REQUEST = 1, KBD_PROTOCOL_RESPONSE = 2, KBD_EVENT = 3 };
#define KBD_VERSION     0x10000
#define KBD_ACCEPTED    1
#define KBD_IS_UNICODE  1
#define KBD_IS_BREAK    2
#define KBD_IS_E0       4
#define KBD_IS_E1       8

struct kbd_msg {
    uint32_t type;
    union {
        uint32_t version;    /* request */
        uint32_t status;     /* response */
        struct {             /* event */
            uint16_t make_code, reserved;
            uint32_t info;
        } PACKED key;
    };
} PACKED;

static volatile bool kbd_answered;
static uint32_t kbd_status;

static void kbd_callback(struct vmbus_channel *ch) {
    uint8_t pkt[256];
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0 || ((struct vmpacket_desc *)pkt)->type != VM_PKT_DATA_INBAND) continue;
        if (vmpacket_datalen(pkt) < 8) continue;
        struct kbd_msg *m = vmpacket_data(pkt);
        if (m->type == KBD_PROTOCOL_RESPONSE) {
            kbd_status = m->status;
            kbd_answered = true;
        } else if (m->type == KBD_EVENT) {
            if (m->key.info & KBD_IS_UNICODE) { /* typed text (e.g. VMConnect's "Type clipboard text") */
                input_unicode(m->key.make_code, !(m->key.info & KBD_IS_BREAK));
                continue;
            }
            if (m->key.info & KBD_IS_E0) input_scancode(0xE0);
            if (m->key.info & KBD_IS_E1) input_scancode(0xE1);
            input_scancode((uint8_t)(m->key.make_code | ((m->key.info & KBD_IS_BREAK) ? 0x80 : 0)));
        }
    }
}

static bool kbd_init(void) {
    struct vmbus_channel *ch = vmbus_find(&kbd_guid, 0);
    if (!ch || vmbus_open(ch, RING_BYTES, RING_BYTES, kbd_callback, NULL)) return false;
    struct kbd_msg req = {.type = KBD_PROTOCOL_REQUEST, .version = KBD_VERSION};
    int st = vmbus_send(ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, &req, 8, 1);
    if (!vmbus_wait(&kbd_answered, 2000) || !(kbd_status & KBD_ACCEPTED)) {
        kprintf("hv-input: keyboard protocol not accepted (send %d, answered %d status %x; out w%u r%u, in w%u r%u)\n",
                st, kbd_answered, kbd_status, ch->out->write_index, ch->out->read_index, ch->in->write_index,
                ch->in->read_index);
        return false;
    }
    return true;
}

/* ---- mouse ---- */

enum { HID_PROTOCOL_REQUEST, HID_PROTOCOL_RESPONSE, HID_DEVICE_INFO, HID_DEVICE_INFO_ACK, HID_INPUT_REPORT };
#define HID_VERSION 0x00020000 /* 2.0 */
#define PIPE_MESSAGE_DATA 1

struct hid_msg {
    uint32_t pipe_type, pipe_size; /* pipe header */
    uint32_t type, size;           /* synthetic HID header */
    uint8_t data[];
} PACKED;

/* one field of the input report */
struct hid_field {
    bool present, relative, is_signed;
    uint8_t report_id;
    uint16_t offset, size; /* in bits, after the report ID byte */
    int32_t min, max;
};

static struct {
    struct vmbus_channel *ch;
    volatile bool answered, have_info;
    bool approved;
    bool uses_ids;
    struct hid_field x, y, wheel, buttons[5];
} ms;

static uint32_t item_u(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_s(const uint8_t *p, int n) {
    uint32_t v = item_u(p, n);
    if (n == 1) return (int8_t)v;
    if (n == 2) return (int16_t)v;
    return (int32_t)v;
}

/* Walk a HID report descriptor and remember where the fields we care about are. */
static void parse_report_descriptor(const uint8_t *d, uint32_t len) {
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16], nusages = 0, umin = 0, umax = 0;
    bool have_range = false;
    uint16_t bitpos[256] = {0};
    for (uint32_t i = 0; i < len;) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) { /* long item */
            if (i + 1 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        int sz = prefix & 3;
        if (sz == 3) sz = 4;
        if (i + 1 + sz > len) break;
        const uint8_t *data = d + i + 1;
        int type = (prefix >> 2) & 3, tag = prefix >> 4;
        i += 1 + sz;
        if (type == 1) { /* global */
            switch (tag) {
            case 0: page = item_u(data, sz); break;
            case 1: lmin = item_s(data, sz); break;
            case 2: lmax = sz < 4 && lmin >= 0 ? (int32_t)item_u(data, sz) : item_s(data, sz); break;
            case 7: rsize = item_u(data, sz); break;
            case 8: rid = item_u(data, sz) & 0xFF; ms.uses_ids = true; break;
            case 9: rcount = item_u(data, sz); break;
            }
        } else if (type == 2) { /* local */
            uint32_t u = item_u(data, sz);
            if (sz < 4) u |= page << 16;
            if (tag == 0 && nusages < ARRAY_SIZE(usages)) usages[nusages++] = u;
            else if (tag == 1) { umin = u; have_range = true; }
            else if (tag == 2) umax = u;
        } else if (type == 0) { /* main */
            if (tag == 8) { /* Input */
                uint32_t flags = item_u(data, sz);
                for (uint32_t k = 0; k < rcount; k++) {
                    uint32_t usage = 0;
                    if (have_range) usage = umin + k <= umax ? umin + k : umax;
                    else if (nusages) usage = usages[k < nusages ? k : nusages - 1];
                    struct hid_field *f = NULL;
                    if (!(flags & 1) && (flags & 2)) { /* data, variable */
                        if (usage == 0x10030) f = &ms.x;
                        else if (usage == 0x10031) f = &ms.y;
                        else if (usage == 0x10038) f = &ms.wheel;
                        else if ((usage >> 16) == 9 && (usage & 0xFFFF) >= 1 && (usage & 0xFFFF) <= 5)
                            f = &ms.buttons[(usage & 0xFFFF) - 1];
                    }
                    if (f && !f->present) {
                        f->present = true;
                        f->relative = flags & 4;
                        f->report_id = (uint8_t)rid;
                        f->offset = bitpos[rid];
                        f->size = (uint16_t)rsize;
                        f->min = lmin;
                        f->max = lmax;
                        f->is_signed = lmin < 0;
                    }
                    bitpos[rid] += (uint16_t)rsize;
                }
            }
            nusages = 0;
            have_range = false;
        }
    }
}

static int32_t field_value(const struct hid_field *f, const uint8_t *r, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < f->size && b < 32; b++) {
        uint32_t bit = f->offset + b;
        if (bit / 8 >= len) break;
        if (r[bit / 8] & (1 << (bit % 8))) v |= 1u << b;
    }
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1)))) v |= ~0u << f->size;
    return (int32_t)v;
}

static void mouse_report(const uint8_t *r, uint32_t len) {
    uint8_t id = 0;
    if (ms.uses_ids) {
        if (!len) return;
        id = *r++;
        len--;
    }
    if (!ms.x.present || ms.x.report_id != id) return;
    struct mouse_event e = {0};
    for (int i = 0; i < 5; i++)
        if (ms.buttons[i].present && field_value(&ms.buttons[i], r, len)) e.buttons |= 1 << i;
    e.buttons &= 7;
    int32_t x = field_value(&ms.x, r, len), y = ms.y.present ? field_value(&ms.y, r, len) : 0;
    if (ms.x.relative) {
        e.dx = x;
        e.dy = y;
    } else {
        int32_t xr = ms.x.max - ms.x.min, yr = ms.y.max - ms.y.min;
        e.absolute = true;
        e.x = xr > 0 ? (int)((int64_t)(x - ms.x.min) * MOUSE_ABS_MAX / xr) : 0;
        e.y = yr > 0 ? (int)((int64_t)(y - ms.y.min) * MOUSE_ABS_MAX / yr) : 0;
    }
    if (ms.wheel.present) e.wheel = -field_value(&ms.wheel, r, len); /* HID: positive is away from the user */
    input_mouse(&e);
}

static void mouse_send(uint32_t type, const void *data, uint32_t len) {
    uint8_t buf[64];
    struct hid_msg *m = (void *)buf;
    m->pipe_type = PIPE_MESSAGE_DATA;
    m->pipe_size = 8 + len;
    m->type = type;
    m->size = len;
    memcpy(m->data, data, len);
    vmbus_send(ms.ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, buf, sizeof *m + len, type + 1);
}

static void mouse_callback(struct vmbus_channel *ch) {
    static uint8_t pkt[4096];
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0 || ((struct vmpacket_desc *)pkt)->type != VM_PKT_DATA_INBAND) continue;
        uint32_t len = vmpacket_datalen(pkt);
        struct hid_msg *m = vmpacket_data(pkt);
        if (len < sizeof *m || m->pipe_type != PIPE_MESSAGE_DATA) continue;
        uint32_t dlen = MIN(m->size, len - (uint32_t)sizeof *m);
        switch (m->type) {
        case HID_PROTOCOL_RESPONSE:
            ms.approved = dlen >= 5 && m->data[4];
            ms.answered = true;
            break;
        case HID_DEVICE_INFO: {
            /* device info (it starts with its own size), then a HID descriptor, then the report descriptor */
            uint32_t info_len = dlen >= 2 ? (m->data[0] | (uint32_t)m->data[1] << 8) : 0;
            if (info_len && dlen >= info_len + 9) {
                const uint8_t *hd = m->data + info_len;
                uint32_t rd_len = hd[7] | (uint32_t)hd[8] << 8;
                if (info_len + hd[0] + rd_len <= dlen) parse_report_descriptor(hd + hd[0], rd_len);
            }
            uint8_t ack = 0;
            mouse_send(HID_DEVICE_INFO_ACK, &ack, 1);
            ms.have_info = true;
            break;
        }
        case HID_INPUT_REPORT: mouse_report(m->data, dlen); break;
        }
    }
}

static bool mouse_init(void) {
    ms.ch = vmbus_find(&mouse_guid, 0);
    if (!ms.ch || vmbus_open(ms.ch, RING_BYTES, RING_BYTES, mouse_callback, NULL)) return false;
    uint32_t version = HID_VERSION;
    mouse_send(HID_PROTOCOL_REQUEST, &version, 4);
    if (!vmbus_wait(&ms.answered, 2000) || !ms.approved) {
        kprintf("hv-input: mouse protocol not accepted\n");
        return false;
    }
    if (!vmbus_wait(&ms.have_info, 2000)) {
        kprintf("hv-input: no mouse device info\n");
        return false;
    }
    if (!ms.x.present) {
        kprintf("hv-input: mouse report has no X axis\n");
        return false;
    }
    input_mouse_attach();
    return true;
}

void hv_input_init(void) {
    if (!vmbus_ready()) return;
    bool k = kbd_init(), m = mouse_init();
    if (k || m)
        kprintf("hv-input: synthetic keyboard %s, mouse %s%s%s\n", k ? "ready" : "missing",
                m ? "ready" : "missing", m && !ms.x.relative ? " (absolute)" : "",
                m && ms.wheel.present ? " (wheel)" : "");
}
