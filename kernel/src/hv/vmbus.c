/* VMBus: version negotiation, channel offers, GPADLs (guest memory shared with the host), opening
   channels and the ring buffers that carry their packets. Control messages arrive through the
   SynIC message slot of SINT2 and are handled in interrupt context; channel interrupts arrive as
   bits (one per channel relid) in SINT2's event flags. Only protocol versions from Windows 8 on
   are supported, so the event flags page is all we need for host-to-guest signalling. */
#include "kernel.h"
#include "arch/cpu.h"
#include "hv/hv.h"
#include "hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/sched.h"

enum {
    CHMSG_OFFER = 1,
    CHMSG_RESCIND = 2,
    CHMSG_REQUEST_OFFERS = 3,
    CHMSG_ALL_OFFERS_DELIVERED = 4,
    CHMSG_OPEN = 5,
    CHMSG_OPEN_RESULT = 6,
    CHMSG_CLOSE = 7,
    CHMSG_GPADL_HEADER = 8,
    CHMSG_GPADL_BODY = 9,
    CHMSG_GPADL_CREATED = 10,
    CHMSG_GPADL_TEARDOWN = 11,
    CHMSG_GPADL_TORNDOWN = 12,
    CHMSG_RELID_RELEASED = 13,
    CHMSG_INITIATE_CONTACT = 14,
    CHMSG_VERSION_RESPONSE = 15,
};

#define VMBUS_CONN_ID_MESSAGE   1
#define VMBUS_CONN_ID_MESSAGE_4 4

struct msg_header {
    uint32_t type;
    uint32_t padding;
} PACKED;

struct msg_offer {
    struct msg_header h;
    struct guid type, instance;
    uint64_t reserved1, reserved2;
    uint16_t chn_flags, mmio_mb;
    uint8_t user_def[120];
    uint16_t sub_index, reserved3;
    uint32_t relid;
    uint8_t monitor_id, monitor_flags;
    uint16_t dedicated_interrupt;
    uint32_t conn_id;
} PACKED;

struct msg_initiate_contact {
    struct msg_header h;
    uint32_t version;
    uint32_t target_vcpu;
    union {
        uint64_t interrupt_page;
        struct {
            uint8_t msg_sint, msg_vtl, reserved[6];
        } PACKED;
    };
    uint64_t monitor_page1, monitor_page2;
} PACKED;

struct msg_version_response {
    struct msg_header h;
    uint8_t supported, state;
    uint16_t padding;
    uint32_t msg_conn_id;
} PACKED;

struct msg_open {
    struct msg_header h;
    uint32_t relid, open_id, gpadl, target_vp, in_page_offset;
    uint8_t user_data[120];
} PACKED;

struct msg_open_result {
    struct msg_header h;
    uint32_t relid, open_id, status;
} PACKED;

struct msg_gpadl_header {
    struct msg_header h;
    uint32_t relid, gpadl;
    uint16_t range_buflen, range_count;
    uint32_t byte_count, byte_offset;
    uint64_t pfn[];
} PACKED;

struct msg_gpadl_body {
    struct msg_header h;
    uint32_t msg_number, gpadl;
    uint64_t pfn[];
} PACKED;

struct msg_gpadl_created {
    struct msg_header h;
    uint32_t relid, gpadl, status;
} PACKED;

#define MAX_RELID 2048
static struct vmbus_channel *channels, **channels_tail = &channels;
static struct vmbus_channel *by_relid[MAX_RELID];
static uint32_t msg_conn_id = VMBUS_CONN_ID_MESSAGE;
static uint32_t next_gpadl = 0xE1E10;
static volatile bool offers_done, connected;
static void (*socket_hook)(struct vmbus_channel *ch);
int vmbus_version_major, vmbus_version_minor;

/* one outstanding request at a time; the interrupt handler fills in the reply */
static struct {
    volatile bool done;
    uint32_t type, key;
    uint8_t reply[HV_MESSAGE_PAYLOAD_BYTES];
} waiter;

static const struct {
    struct guid g;
    const char *name;
} known[] = {
    {GUID_INIT(0xf912ad6d, 0x2b17, 0x48ea, 0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84), "keyboard"},
    {GUID_INIT(0xcfa8b69e, 0x5b4a, 0x4cc0, 0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a), "mouse"},
    {GUID_INIT(0xda0a7802, 0xe377, 0x4aac, 0x8e, 0x77, 0x05, 0x58, 0xeb, 0x10, 0x73, 0xf8), "video"},
    {GUID_INIT(0xba6163d9, 0x04a1, 0x4d29, 0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f), "SCSI"},
    {GUID_INIT(0x32412632, 0x86cb, 0x44a2, 0x9b, 0x5c, 0x50, 0xd1, 0x41, 0x73, 0x54, 0xf5), "IDE"},
    {GUID_INIT(0xf8615163, 0xdf3e, 0x46c5, 0x91, 0x3f, 0xf2, 0xd2, 0xf9, 0x65, 0xed, 0x0e), "network"},
    {GUID_INIT(0x0e0b6031, 0x5213, 0x4934, 0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb), "shutdown"},
    {GUID_INIT(0x9527e630, 0xd0ae, 0x497b, 0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf), "time sync"},
    {GUID_INIT(0x57164f39, 0x9115, 0x4e78, 0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d), "heartbeat"},
    {GUID_INIT(0xa9a0f4e7, 0x5a45, 0x4d96, 0xb8, 0x27, 0x8a, 0x84, 0x1e, 0x8c, 0x03, 0xe6), "KVP"},
    {GUID_INIT(0x35fa2e29, 0xea23, 0x4236, 0x96, 0xae, 0x3a, 0x6e, 0xba, 0xcb, 0xa4, 0x40), "VSS"},
    {GUID_INIT(0xf8e65716, 0x3cb3, 0x4a06, 0x9a, 0x60, 0x18, 0x89, 0xc5, 0xcc, 0xca, 0xb5), "reserved"},
    {GUID_INIT(0x3375baf4, 0x9e15, 0x4b30, 0xb7, 0x65, 0x67, 0xac, 0xb1, 0x0d, 0x60, 0x7b), "reserved"},
    {GUID_INIT(0x525074dc, 0x8985, 0x46e2, 0x80, 0x57, 0xa3, 0x07, 0xdc, 0x18, 0xa5, 0x02), "dynamic memory"},
    {GUID_INIT(0x34d14be3, 0xdee4, 0x41c8, 0x9a, 0xe7, 0x6b, 0x17, 0x49, 0x77, 0xc1, 0x92), "guest services"},
    {GUID_INIT(0x276aacf4, 0xac15, 0x426c, 0x98, 0xdd, 0x75, 0x21, 0xad, 0x3f, 0x01, 0xfe), "remote desktop"},
    {GUID_INIT(0x44c4f61d, 0x4444, 0x4400, 0x9d, 0x52, 0x80, 0x2e, 0x27, 0xed, 0xe1, 0x9f), "PCI pass-through"},
};

void guid_format(const struct guid *g, char *out) {
    const uint8_t *b = g->b;
    ksnprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1],
              b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static const char *guid_name(const struct guid *g) {
    for (size_t i = 0; i < ARRAY_SIZE(known); i++)
        if (!memcmp(&known[i].g, g, sizeof *g)) return known[i].name;
    return NULL;
}

/* ---- control messages (interrupt context) ---- */

static void on_offer(const struct msg_offer *o) {
    if (o->relid >= MAX_RELID) return;
    struct vmbus_channel *ch = kzalloc(sizeof *ch);
    if (!ch) return;
    ch->type = o->type;
    ch->instance = o->instance;
    ch->chn_flags = o->chn_flags;
    ch->mmio_mb = o->mmio_mb;
    memcpy(ch->user_def, o->user_def, sizeof ch->user_def);
    ch->sub_index = o->sub_index;
    ch->relid = o->relid;
    ch->conn_id = o->conn_id;
    by_relid[o->relid] = ch;
    *channels_tail = ch;
    channels_tail = &ch->next;
    if (offers_done && (ch->chn_flags & VMBUS_CHANNEL_SOCKET)) {
        if (socket_hook) socket_hook(ch);
    } else if (offers_done) {
        char gs[37], is[37];
        guid_format(&ch->type, gs);
        guid_format(&ch->instance, is);
        kprintf("vmbus: new offer %s instance %s relid %u flags %x\n", gs, is, ch->relid, ch->chn_flags);
    }
}

/* Forget a rescinded channel and hand its relid back to the host. Interrupts off. */
static void free_channel(struct vmbus_channel *ch) {
    uint32_t m[2 + 1] = {CHMSG_RELID_RELEASED, 0, ch->relid};
    hv_post_message(msg_conn_id, 1, m, sizeof m);
    by_relid[ch->relid] = NULL;
    for (struct vmbus_channel **pp = &channels; *pp; pp = &(*pp)->next) {
        if (*pp == ch) {
            *pp = ch->next;
            if (channels_tail == &ch->next) channels_tail = pp;
            break;
        }
    }
    kfree(ch);
}

static void handle_message(const uint8_t *p, uint32_t len) {
    uint32_t type = ((const struct msg_header *)p)->type;
    uint32_t key = 0;
    switch (type) {
    case CHMSG_OFFER: on_offer((const struct msg_offer *)p); return;
    case CHMSG_ALL_OFFERS_DELIVERED: offers_done = true; return;
    case CHMSG_RESCIND: {
        uint32_t relid = ((const uint32_t *)p)[2];
        struct vmbus_channel *ch = relid < MAX_RELID ? by_relid[relid] : NULL;
        if (!ch) return;
        ch->rescinded = true;
        if (!(ch->chn_flags & VMBUS_CHANNEL_SOCKET)) kprintf("vmbus: channel %u rescinded\n", relid);
        if (ch->on_rescind) ch->on_rescind(ch);
        if (!ch->claimed && !ch->opened) free_channel(ch);
        return;
    }
    case CHMSG_OPEN_RESULT: key = ((const struct msg_open_result *)p)->relid; break;
    case CHMSG_GPADL_CREATED: key = ((const struct msg_gpadl_created *)p)->gpadl; break;
    case CHMSG_GPADL_TORNDOWN: key = ((const uint32_t *)p)[2]; break;
    case CHMSG_VERSION_RESPONSE: break;
    default: return;
    }
    if (!waiter.done && waiter.type == type && waiter.key == key) {
        memcpy(waiter.reply, p, MIN(len, sizeof waiter.reply));
        waiter.done = true;
    }
}

static void vmbus_isr(void) {
    struct hv_message *m = hv_message_slot(VMBUS_MESSAGE_SINT);
    while (m->type != 0) {
        uint8_t buf[HV_MESSAGE_PAYLOAD_BYTES];
        uint32_t len = MIN(m->payload_size, sizeof buf);
        memcpy(buf, m->payload, len);
        hv_message_done(m);
        if (len >= sizeof(struct msg_header)) handle_message(buf, len);
    }
    volatile uint64_t *ev = hv_event_flags(VMBUS_MESSAGE_SINT);
    for (int w = 0; w < MAX_RELID / 64; w++) {
        if (!ev[w]) continue;
        uint64_t bits = __atomic_exchange_n(&ev[w], 0, __ATOMIC_SEQ_CST);
        while (bits) {
            int b = __builtin_ctzll(bits);
            bits &= bits - 1;
            struct vmbus_channel *ch = by_relid[w * 64 + b];
            if (ch && ch->opened && ch->callback) ch->callback(ch);
        }
    }
}

/* ---- requests ---- */

bool vmbus_wait(volatile bool *flag, uint64_t ms) {
    uint64_t end = uptime_ms() + ms;
    while (!*flag) {
        if (uptime_ms() >= end) return false;
        sti();
        hlt();
    }
    return true;
}

static uint16_t post(const void *msg, uint32_t len) {
    uint16_t st = 0;
    for (int tries = 0; tries < 100; tries++) {
        st = hv_post_message(msg_conn_id, 1, msg, len);
        if (st != HV_STATUS_INSUFFICIENT_BUFFERS) break;
        sleep_ms(1);
    }
    return st;
}

/* Post a message and wait for the reply of `type` carrying `key`. */
static bool request(const void *msg, uint32_t len, uint32_t type, uint32_t key, uint64_t ms) {
    waiter.type = type;
    waiter.key = key;
    waiter.done = false;
    uint16_t st = post(msg, len);
    if (st != HV_STATUS_SUCCESS) {
        kprintf("vmbus: post message failed (status %x)\n", st);
        return false;
    }
    return vmbus_wait(&waiter.done, ms);
}

static bool try_version(uint32_t version) {
    struct msg_initiate_contact m;
    memset(&m, 0, sizeof m);
    m.h.type = CHMSG_INITIATE_CONTACT;
    m.version = version;
    m.target_vcpu = hv_vp_index;
    if (version >= 0x50000) {
        m.msg_sint = VMBUS_MESSAGE_SINT;
        msg_conn_id = VMBUS_CONN_ID_MESSAGE_4;
    } else {
        m.interrupt_page = pmm_alloc_zeroed(); /* only used before Windows 8, but must be given */
        msg_conn_id = VMBUS_CONN_ID_MESSAGE;
    }
    m.monitor_page1 = pmm_alloc_zeroed();
    m.monitor_page2 = pmm_alloc_zeroed();
    if (!request(&m, sizeof m, CHMSG_VERSION_RESPONSE, 0, 1000)) return false;
    struct msg_version_response *r = (void *)waiter.reply;
    if (!r->supported) return false;
    if (version >= 0x50000 && r->msg_conn_id) msg_conn_id = r->msg_conn_id;
    vmbus_version_major = version >> 16;
    vmbus_version_minor = version & 0xFFFF;
    return true;
}

bool vmbus_init(void) {
    if (!hv_core_init(vmbus_isr)) return false;
    /* newest first: 5.3 .. 5.0 (Windows 10 1809+), 4.1, 4.0 (Windows 10), 3.0 (8.1), 2.4 (8) */
    static const uint32_t versions[] = {0x50003, 0x50002, 0x50001, 0x50000, 0x40001, 0x40000, 0x30000, 0x20004};
    for (size_t i = 0; i < ARRAY_SIZE(versions) && !connected; i++) connected = try_version(versions[i]);
    if (!connected) {
        kprintf("vmbus: the host accepted no protocol version\n");
        return false;
    }
    struct msg_header req = {CHMSG_REQUEST_OFFERS, 0};
    if (post(&req, sizeof req) != HV_STATUS_SUCCESS || !vmbus_wait(&offers_done, 3000))
        kprintf("vmbus: offers did not complete\n");
    int n = 0;
    for (struct vmbus_channel *ch = channels; ch; ch = ch->next) n++;
    kprintf("vmbus: protocol %d.%d, %d offers\n", vmbus_version_major, vmbus_version_minor, n);
    for (struct vmbus_channel *ch = channels; ch; ch = ch->next) {
        char gs[37];
        guid_format(&ch->type, gs);
        const char *name = guid_name(&ch->type);
        kprintf("  relid %-3u %s %s%s\n", ch->relid, gs, name ? name : "?",
                (ch->chn_flags & 0x2000) ? " (socket)" : "");
    }
    return true;
}

bool vmbus_ready(void) { return connected; }

struct vmbus_channel *vmbus_find(const struct guid *type, int n) {
    for (struct vmbus_channel *ch = channels; ch; ch = ch->next)
        if (!ch->rescinded && !memcmp(&ch->type, type, sizeof *type) && n-- == 0) return ch;
    return NULL;
}

uint32_t vmbus_gpadl_create(struct vmbus_channel *ch, uint64_t pa, uint32_t pages) {
    uint8_t buf[HV_MESSAGE_PAYLOAD_BYTES];
    uint32_t gpadl = __atomic_fetch_add(&next_gpadl, 1, __ATOMIC_SEQ_CST);
    const uint32_t first_max = (sizeof buf - sizeof(struct msg_gpadl_header)) / 8;
    const uint32_t body_max = (sizeof buf - sizeof(struct msg_gpadl_body)) / 8;

    struct msg_gpadl_header *h = (void *)buf;
    memset(buf, 0, sizeof buf);
    h->h.type = CHMSG_GPADL_HEADER;
    h->relid = ch->relid;
    h->gpadl = gpadl;
    h->range_count = 1;
    h->range_buflen = (uint16_t)(8 + pages * 8);
    h->byte_count = pages * 4096;
    h->byte_offset = 0;
    uint32_t n = MIN(pages, first_max);
    for (uint32_t i = 0; i < n; i++) h->pfn[i] = (pa >> 12) + i;
    uint32_t sent = n;

    waiter.type = CHMSG_GPADL_CREATED;
    waiter.key = gpadl;
    waiter.done = false;
    if (post(buf, sizeof *h + n * 8) != HV_STATUS_SUCCESS) return 0;
    while (sent < pages) {
        struct msg_gpadl_body *b = (void *)buf;
        memset(buf, 0, sizeof buf);
        b->h.type = CHMSG_GPADL_BODY;
        b->gpadl = gpadl;
        n = MIN(pages - sent, body_max);
        for (uint32_t i = 0; i < n; i++) b->pfn[i] = (pa >> 12) + sent + i;
        sent += n;
        if (post(buf, sizeof *b + n * 8) != HV_STATUS_SUCCESS) return 0;
    }
    if (!vmbus_wait(&waiter.done, 3000)) {
        kprintf("vmbus: GPADL %x for relid %u timed out\n", gpadl, ch->relid);
        return 0;
    }
    struct msg_gpadl_created *r = (void *)waiter.reply;
    if (r->status) {
        kprintf("vmbus: GPADL %x for relid %u failed (%x)\n", gpadl, ch->relid, r->status);
        return 0;
    }
    return gpadl;
}

int vmbus_open(struct vmbus_channel *ch, uint32_t out_bytes, uint32_t in_bytes, vmbus_callback_t cb, void *priv) {
    if (ch->opened) return -EBUSY;
    ch->ring_pages_out = (uint32_t)(ALIGN_UP(out_bytes, PAGE_SIZE) / PAGE_SIZE) + 1;
    ch->ring_pages_in = (uint32_t)(ALIGN_UP(in_bytes, PAGE_SIZE) / PAGE_SIZE) + 1;
    uint32_t total = ch->ring_pages_out + ch->ring_pages_in;
    uint64_t pa = pmm_alloc_contig(total);
    if (!pa) return -ENOMEM;
    ch->ring = phys_to_virt(pa);
    memset(ch->ring, 0, total * PAGE_SIZE);
    ch->out = (struct hv_ring *)ch->ring;
    ch->out_data = ch->ring + PAGE_SIZE;
    ch->out_size = (ch->ring_pages_out - 1) * PAGE_SIZE;
    ch->in = (struct hv_ring *)(ch->ring + ch->ring_pages_out * PAGE_SIZE);
    ch->in_data = (uint8_t *)ch->in + PAGE_SIZE;
    ch->in_size = (ch->ring_pages_in - 1) * PAGE_SIZE;
    ch->out->feature_bits = 1; /* we honour pending_send_sz */
    ch->in->feature_bits = 1;
    ch->callback = cb;
    ch->priv = priv;

    ch->ring_gpadl = vmbus_gpadl_create(ch, pa, total);
    if (!ch->ring_gpadl) return -EIO;

    struct msg_open m;
    memset(&m, 0, sizeof m);
    m.h.type = CHMSG_OPEN;
    m.relid = ch->relid;
    m.open_id = ch->relid;
    m.gpadl = ch->ring_gpadl;
    m.target_vp = hv_vp_index;
    m.in_page_offset = ch->ring_pages_out;
    ch->opened = true; /* events may arrive as soon as the host opens its side */
    if (!request(&m, sizeof m, CHMSG_OPEN_RESULT, ch->relid, 3000)) {
        ch->opened = false;
        kprintf("vmbus: open of relid %u timed out\n", ch->relid);
        return -ETIMEDOUT;
    }
    struct msg_open_result *r = (void *)waiter.reply;
    if (r->status) {
        ch->opened = false;
        kprintf("vmbus: open of relid %u failed (%x)\n", ch->relid, r->status);
        return -EIO;
    }
    return 0;
}

void vmbus_gpadl_teardown(struct vmbus_channel *ch, uint32_t gpadl) {
    uint32_t m[2 + 2] = {CHMSG_GPADL_TEARDOWN, 0, ch->relid, gpadl};
    if (!request(m, sizeof m, CHMSG_GPADL_TORNDOWN, gpadl, 3000))
        kprintf("vmbus: GPADL %x teardown timed out\n", gpadl);
}

void vmbus_close(struct vmbus_channel *ch) {
    if (!ch->opened) return;
    uint64_t f = irq_save();
    ch->opened = false; /* no more callbacks */
    irq_restore(f);
    bool ok = true;
    if (!ch->rescinded) {
        uint32_t m[2 + 1] = {CHMSG_CLOSE, 0, ch->relid};
        post(m, sizeof m);
        uint32_t t[2 + 2] = {CHMSG_GPADL_TEARDOWN, 0, ch->relid, ch->ring_gpadl};
        ok = request(t, sizeof t, CHMSG_GPADL_TORNDOWN, ch->ring_gpadl, 3000);
        if (!ok) kprintf("vmbus: GPADL %x teardown timed out\n", ch->ring_gpadl);
    }
    /* if the teardown failed the host may still write there: leak the pages rather than reuse them */
    if (ok) {
        uint64_t pa = hhdm_virt_to_phys(ch->ring);
        for (uint32_t i = 0; i < ch->ring_pages_out + ch->ring_pages_in; i++) pmm_free(pa + (uint64_t)i * PAGE_SIZE);
    }
    ch->ring = NULL;
    ch->ring_gpadl = 0;
}

void vmbus_release(struct vmbus_channel *ch) {
    uint64_t f = irq_save();
    ch->claimed = false;
    ch->callback = NULL;
    ch->on_rescind = NULL;
    if (ch->rescinded && !ch->opened) free_channel(ch);
    irq_restore(f);
}

void vmbus_set_socket_hook(void (*hook)(struct vmbus_channel *ch)) { socket_hook = hook; }

/* ---- ring buffers ---- */

static void ring_copy_in(struct vmbus_channel *ch, uint32_t *pos, const void *src, uint32_t len) {
    const uint8_t *s = src;
    while (len) {
        uint32_t n = MIN(len, ch->out_size - *pos);
        if (s) memcpy(ch->out_data + *pos, s, n);
        else memset(ch->out_data + *pos, 0, n);
        if (s) s += n;
        len -= n;
        *pos = (*pos + n) % ch->out_size;
    }
}

static int ring_write(struct vmbus_channel *ch, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    if (!ch->opened || ch->rescinded) return -EIO;
    uint32_t len = alen + blen;
    uint32_t aligned = (uint32_t)ALIGN_UP(len, 8);
    uint32_t need = aligned + 8;
    uint64_t f = irq_save();
    uint32_t w = ch->out->write_index, r = ch->out->read_index;
    uint32_t avail = w >= r ? ch->out_size - (w - r) : r - w;
    if (avail <= need) {
        irq_restore(f);
        return -EAGAIN;
    }
    uint32_t pos = w;
    ring_copy_in(ch, &pos, a, alen);
    if (blen) ring_copy_in(ch, &pos, b, blen);
    if (aligned > len) ring_copy_in(ch, &pos, NULL, aligned - len);
    uint64_t prev = (uint64_t)w << 32;
    ring_copy_in(ch, &pos, &prev, 8);
    __asm__ volatile("" ::: "memory");
    ch->out->write_index = pos;
    hv_mb();
    bool signal = !ch->out->interrupt_mask && ch->out->read_index == w;
    irq_restore(f);
    if (signal) {
        uint16_t st = hv_signal_event(ch->conn_id);
        if (st) kprintf("vmbus: signal on connection %u failed (%x)\n", ch->conn_id, st);
    }
    return 0;
}

int vmbus_send(struct vmbus_channel *ch, uint16_t type, uint16_t flags, const void *data, uint32_t len,
               uint64_t trans_id) {
    struct vmpacket_desc d = {type, 2, (uint16_t)((sizeof d + len + 7) / 8), flags, trans_id};
    return ring_write(ch, &d, sizeof d, data, len);
}

int vmbus_send_raw(struct vmbus_channel *ch, const void *hdr, uint32_t hdr_len, const void *data, uint32_t len) {
    return ring_write(ch, hdr, hdr_len, data, len);
}

static void ring_copy_out(struct vmbus_channel *ch, uint32_t pos, void *dst, uint32_t len) {
    uint8_t *d = dst;
    while (len) {
        uint32_t n = MIN(len, ch->in_size - pos);
        memcpy(d, ch->in_data + pos, n);
        d += n;
        len -= n;
        pos = (pos + n) % ch->in_size;
    }
}

int vmbus_recv(struct vmbus_channel *ch, void *buf, uint32_t len) {
    if (!ch->opened) return 0;
    uint64_t f = irq_save();
    uint32_t r = ch->in->read_index, w = ch->in->write_index;
    if (r == w) {
        irq_restore(f);
        return 0;
    }
    __asm__ volatile("" ::: "memory");
    uint32_t avail = w >= r ? w - r : ch->in_size - r + w;
    struct vmpacket_desc d;
    ring_copy_out(ch, r, &d, sizeof d);
    uint32_t plen = d.len8 * 8u;
    if (plen < sizeof d || avail < plen + 8) {
        irq_restore(f);
        return 0;
    }
    int ret = (int)plen;
    if (plen <= len) ring_copy_out(ch, r, buf, plen);
    else ret = -E2BIG;
    __asm__ volatile("" ::: "memory");
    ch->in->read_index = (r + plen + 8) % ch->in_size;
    hv_mb();
    /* the host may be waiting for room to write its next packet */
    bool signal = ch->in->pending_send_sz != 0;
    irq_restore(f);
    if (signal) hv_signal_event(ch->conn_id);
    return ret;
}
