/* Hyper-V synthetic SCSI controller (storvsc): SCSI commands carried over VMBus. Generation 2 VMs
   keep every disk and DVD drive here. The driver of the VMBus channels of the SCSI type.

   Reads and writes go to the host as GPA-direct packets naming the request's own pages: the
   request's segments are page runs (the page-size boundary joins them), and none is copied. Each
   request holds a slot of the controller's table, its transaction number the slot's number: any
   number of requests are under way at once, and the reply comes in the channel's callback (the
   interrupt handler of the CPU VMBus signals), which ends the request there. The protocol's opening
   and the identification of the LUNs go one at a time through a bounce page, before any disk is
   listed.

   A request the host has not answered in REQUEST_NS (twice the time the packet tells it to take)
   means the host cannot be told apart from one that still reads the pages: the channel is closed,
   after which it reads nothing, and every request still in a slot fails with the disks marked gone. */
#include "kernel.h"
#include "arch/spin.h"
#include "dev/blk.h"
#include "hal/hal.h"
#include "vm/hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

static const struct guid scsi_guid =
    GUID_INIT(0xba6163d9, 0x04a1, 0x4d29, 0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f);

enum {
    VSTOR_COMPLETE_IO = 1,
    VSTOR_EXECUTE_SRB = 3,
    VSTOR_BEGIN_INIT = 7,
    VSTOR_END_INIT = 8,
    VSTOR_QUERY_PROTOCOL = 9,
    VSTOR_QUERY_PROPERTIES = 10,
};
#define VSTOR_REQUEST_COMPLETION 1

#define SRB_STATUS_SUCCESS 0x01
#define SRB_FLAGS_QUEUE_ACTION_ENABLE 0x02
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x08
#define SRB_FLAGS_DATA_IN  0x40
#define SRB_FLAGS_DATA_OUT 0x80
enum { DIR_OUT = 0, DIR_IN = 1, DIR_NONE = 2 }; /* the protocol's data_in values */

#define SRB_TIMEOUT_S 60
#define REQUEST_NS (2ull * SRB_TIMEOUT_S * NS_PER_S)
#define CONTROL_NS (10ull * NS_PER_S)

struct vmscsi_request {
    uint16_t length;
    uint8_t srb_status, scsi_status;
    uint8_t port, path_id, target_id, lun;
    uint8_t cdb_length, sense_info_length, data_in, reserved;
    uint32_t data_transfer_length;
    uint8_t cdb[20]; /* also the sense data on return */
    /* Windows 8 extension */
    uint16_t reserved2;
    uint8_t queue_tag, queue_action;
    uint32_t srb_flags, timeout, queue_sort_key;
} PACKED;

struct vstor_packet {
    uint32_t op, flags, status;
    union {
        struct vmscsi_request srb;
        struct {
            uint16_t major_minor, revision;
        } PACKED version;
        struct {
            uint32_t reserved;
            uint16_t max_channels, reserved1;
            uint32_t flags, max_transfer;
            uint64_t reserved2;
        } PACKED props;
        uint8_t raw[52];
    };
} PACKED;

#define MOST_BYTES (128u * 1024) /* one request */
#define MOST_PAGES (MOST_BYTES / PAGE_SIZE + 1)
#define SLOTS 64
#define LUNS 64
#define SYNC_ID (1ull << 63) /* a control exchange's transaction number: the rest are slots' + 1 */

struct gpa_header {
    struct vmpacket_desc d;
    uint32_t reserved, range_count;
    uint32_t len, offset;
    uint64_t pfn[MOST_PAGES];
} PACKED;

struct slot {
    struct blk_request *request; /* NULL: free */
    uint64_t deadline;
};

struct scsi_disk;
struct storvsc {
    struct vmbus_channel *ch;
    struct hal_device *hal;
    /* The slots, under lock (the callback takes it too): which request is under way where. freed
       moves on as one is given back (room's word). */
    struct spinlock lock;
    struct slot slots[SLOTS];
    uint64_t freed;
    struct wait_queue room;
    bool dead;     /* atomic: the channel is closed and every request ended */
    bool stopping; /* atomic: the watchdog ends */
    /* The opening and the LUNs' identification: one control exchange at a time, the bounce page its
       data. The callback (one at a time) writes reply for the exchange numbered tid, puts its
       number in answered, counts replies up and wakes wq; the exchange reads replies before it
       looks at answered and sleeps only while replies still reads that (wq_wait_unless). */
    struct mutex control;
    uint64_t tid, answered, replies;
    struct wait_queue wq;
    struct vstor_packet reply;
    uint8_t *bounce;
    uint64_t bounce_pa;
    uint32_t max_sectors;
    struct scsi_disk *disks[LUNS];
    unsigned ndisks;
};

struct scsi_disk {
    struct blkdev blk;
    struct storvsc *hba;
    uint8_t lun;
};

static int status_of(const struct vstor_packet *reply) {
    return reply->status || (reply->srb.srb_status & 0x3F) != SRB_STATUS_SUCCESS || reply->srb.scsi_status
               ? -N_ERR_IO
               : 0;
}

/* The request in slot id, taken out of it: NULL when it is not there (the controller ended it). Whoever
   gets it ends it. */
static struct blk_request *slot_claim(struct storvsc *s, unsigned id) {
    uint64_t f = spin_lock(&s->lock);
    struct blk_request *r = s->slots[id].request;
    s->slots[id].request = NULL;
    spin_unlock(&s->lock, f);
    if (!r) return NULL;
    __atomic_add_fetch(&s->freed, 1, __ATOMIC_RELEASE);
    wq_wake_all(&s->room);
    return r;
}

static void storvsc_callback(struct vmbus_channel *ch) {
    struct storvsc *s = ch->priv;
    uint8_t pkt[256];
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0) continue;
        struct vmpacket_desc *d = (void *)pkt;
        if (d->type != VM_PKT_COMP) continue; /* bus change notices are ignored */
        if (d->trans_id & SYNC_ID) {
            if (d->trans_id != __atomic_load_n(&s->tid, __ATOMIC_ACQUIRE)) continue; /* a late reply to an exchange given up on */
            memset(&s->reply, 0, sizeof s->reply);
            memcpy(&s->reply, vmpacket_data(pkt), MIN(vmpacket_datalen(pkt), sizeof s->reply));
            __atomic_store_n(&s->answered, d->trans_id, __ATOMIC_RELEASE);
            __atomic_add_fetch(&s->replies, 1, __ATOMIC_SEQ_CST);
            wq_wake_all(&s->wq);
            continue;
        }
        if (!d->trans_id || d->trans_id > SLOTS) continue;
        struct vstor_packet reply;
        memset(&reply, 0, sizeof reply);
        memcpy(&reply, vmpacket_data(pkt), MIN(vmpacket_datalen(pkt), sizeof reply));
        struct blk_request *r = slot_claim(s, (unsigned)d->trans_id - 1);
        if (r) blk_complete(r, status_of(&reply));
    }
}

/* ---- control exchanges ---- */
/* Until the reply to exchange tid has come, or the deadline: false when it did not. */
static bool reply_wait(struct storvsc *s, uint64_t tid, uint64_t deadline) {
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->replies, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s->answered, __ATOMIC_ACQUIRE) == tid) return true;
        if (clock_now_ns() >= deadline) return false;
        wq_wait_unless(&s->wq, &s->replies, seen, deadline);
    }
}

/* Send a packet (with the bounce page attached if len > 0) and wait for its completion. The
   control lock held. */
static int exchange(struct storvsc *s, struct vstor_packet *p, uint32_t len) {
    uint64_t tid = SYNC_ID | ((s->tid + 1) & ~SYNC_ID);
    __atomic_store_n(&s->tid, tid, __ATOMIC_RELEASE); /* before the packet: its reply finds it */
    int r;
    if (len) {
        struct gpa_header h;
        memset(&h, 0, sizeof h);
        uint32_t hlen = (uint32_t)(offsetof(struct gpa_header, pfn) + 8);
        h.d.type = VM_PKT_DATA_GPA_DIRECT;
        h.d.offset8 = (uint16_t)(hlen / 8);
        h.d.len8 = (uint16_t)((hlen + sizeof *p + 7) / 8);
        h.d.flags = VMBUS_FLAG_COMPLETION_REQUESTED;
        h.d.trans_id = tid;
        h.range_count = 1;
        h.len = len;
        h.pfn[0] = s->bounce_pa >> 12;
        r = vmbus_send_raw(s->ch, &h, hlen, p, sizeof *p);
    } else {
        r = vmbus_send(s->ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, p, sizeof *p, tid);
    }
    if (r < 0) return r;
    if (!reply_wait(s, tid, clock_after(CONTROL_NS))) {
        kprintf("storvsc: request timed out\n");
        return -N_ERR_TIMEOUT;
    }
    return 0;
}

static int control(struct storvsc *s, uint32_t op, struct vstor_packet *p) {
    p->op = op;
    p->flags = VSTOR_REQUEST_COMPLETION;
    int r = exchange(s, p, 0);
    if (r) return r;
    return s->reply.status ? -N_ERR_IO : 0;
}

/* One SCSI command against `lun` whose data (at most a page) goes through the bounce page. */
static int scsi(struct storvsc *s, uint8_t lun, const uint8_t *cdb, uint8_t cdb_len, int dir, uint32_t len) {
    struct vstor_packet p;
    memset(&p, 0, sizeof p);
    p.op = VSTOR_EXECUTE_SRB;
    p.flags = VSTOR_REQUEST_COMPLETION;
    p.srb.length = sizeof p.srb;
    p.srb.lun = lun;
    p.srb.cdb_length = cdb_len;
    p.srb.sense_info_length = 20;
    p.srb.data_in = (uint8_t)dir;
    p.srb.data_transfer_length = len;
    memcpy(p.srb.cdb, cdb, cdb_len);
    p.srb.srb_flags = SRB_FLAGS_QUEUE_ACTION_ENABLE | SRB_FLAGS_DISABLE_SYNCH_TRANSFER |
                      (dir == DIR_IN ? SRB_FLAGS_DATA_IN : dir == DIR_OUT ? SRB_FLAGS_DATA_OUT : 0);
    p.srb.timeout = SRB_TIMEOUT_S;
    int r = exchange(s, &p, dir == DIR_NONE ? 0 : len);
    return r ? r : status_of(&s->reply);
}

/* INQUIRY and READ CAPACITY(10) of one LUN, the control lock held: 0 with its model and size, or
   nonzero when it is no 512-byte disk. */
static int identify(struct storvsc *s, uint8_t lun, char model[41], uint64_t *sectors) {
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    if (scsi(s, lun, inq, 6, DIR_IN, 36)) return -1;
    uint8_t type = s->bounce[0];
    if ((type & 0xE0) || (type & 0x1F) != 0) return -1; /* not present, or not a disk (e.g. the DVD drive) */
    memcpy(model, s->bounce + 8, 8);
    model[8] = ' ';
    memcpy(model + 9, s->bounce + 16, 16);
    model[25] = 0;
    for (int i = 24; i >= 0 && (model[i] == ' ' || !model[i]); i--) model[i] = 0;
    uint8_t cap[10] = {0x25};
    if (scsi(s, lun, cap, 10, DIR_IN, 8)) return -1;
    uint8_t *c = s->bounce;
    uint64_t last = (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
    uint32_t bsize = (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7];
    if (bsize != 512) {
        kprintf("storvsc: LUN %u has %u-byte sectors, skipped\n", lun, bsize);
        return -1;
    }
    *sectors = last + 1;
    return 0;
}

/* The protocol's opening (begin, version, properties, end), the control lock held: 0 when the
   host took it. */
static int negotiate(struct storvsc *s) {
    struct vstor_packet p;
    memset(&p, 0, sizeof p);
    if (control(s, VSTOR_BEGIN_INIT, &p)) {
        kprintf("storvsc: begin-initialization failed\n");
        return -1;
    }
    static const uint16_t versions[] = {0x0602, 0x0600, 0x0501}; /* Windows 10, 8.1, 8 */
    bool ok = false;
    for (size_t i = 0; i < ARRAY_SIZE(versions) && !ok; i++) {
        memset(&p, 0, sizeof p);
        p.version.major_minor = versions[i];
        ok = control(s, VSTOR_QUERY_PROTOCOL, &p) == 0;
    }
    if (!ok) {
        kprintf("storvsc: no protocol version accepted\n");
        return -1;
    }
    memset(&p, 0, sizeof p);
    uint32_t max_transfer = MOST_BYTES;
    if (control(s, VSTOR_QUERY_PROPERTIES, &p) == 0 && s->reply.props.max_transfer)
        max_transfer = MIN(max_transfer, s->reply.props.max_transfer);
    s->max_sectors = max_transfer / 512;
    memset(&p, 0, sizeof p);
    if (control(s, VSTOR_END_INIT, &p)) {
        kprintf("storvsc: end-initialization failed\n");
        return -1;
    }
    return 0;
}

/* ---- requests ---- */
/* A free slot for r, waiting for one while all are under way: -1 once the controller is dead. */
static int slot_take(struct storvsc *s, struct blk_request *r) {
    for (;;) {
        uint64_t f = spin_lock(&s->lock);
        if (__atomic_load_n(&s->dead, __ATOMIC_ACQUIRE)) {
            spin_unlock(&s->lock, f);
            return -1;
        }
        for (unsigned i = 0; i < SLOTS; i++)
            if (!s->slots[i].request) {
                s->slots[i].request = r;
                s->slots[i].deadline = clock_after(REQUEST_NS);
                spin_unlock(&s->lock, f);
                return (int)i;
            }
        uint64_t seen = __atomic_load_n(&s->freed, __ATOMIC_ACQUIRE);
        spin_unlock(&s->lock, f);
        wq_wait_unless(&s->room, &s->freed, seen, CLOCK_NEVER);
    }
}

/* r as a packet of its own: READ(10), WRITE(10) or SYNCHRONIZE CACHE(10) (SBC) for its LUN, with the
   pages of its segments for the host to move. False when the ring would not take it for good. */
static int packet_send(struct scsi_disk *d, struct blk_request *r, unsigned id) {
    struct storvsc *s = d->hba;
    struct vstor_packet p;
    memset(&p, 0, sizeof p);
    p.op = VSTOR_EXECUTE_SRB;
    p.flags = VSTOR_REQUEST_COMPLETION;
    p.srb.length = sizeof p.srb;
    p.srb.lun = d->lun;
    p.srb.cdb_length = 10;
    p.srb.sense_info_length = 20;
    p.srb.srb_flags = SRB_FLAGS_QUEUE_ACTION_ENABLE | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
    p.srb.timeout = SRB_TIMEOUT_S;
    uint32_t n = r->sectors;
    uint64_t lba = r->lba;
    p.srb.data_in = DIR_NONE;
    if (r->op != BLK_PERSIST) {
        uint8_t cdb[10] = {r->op == BLK_WRITE ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0};
        memcpy(p.srb.cdb, cdb, sizeof cdb);
        p.srb.data_in = r->op == BLK_WRITE ? DIR_OUT : DIR_IN;
        p.srb.data_transfer_length = n * 512;
        p.srb.srb_flags |= r->op == BLK_WRITE ? SRB_FLAGS_DATA_OUT : SRB_FLAGS_DATA_IN;
    } else {
        p.srb.cdb[0] = 0x35;
    }
    for (;;) {
        int e;
        if (r->op == BLK_PERSIST) {
            e = vmbus_send(s->ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, &p, sizeof p, id + 1ull);
        } else {
            struct gpa_header h;
            memset(&h, 0, offsetof(struct gpa_header, pfn));
            unsigned pages = 0;
            for (uint32_t i = 0; i < r->segment_count; i++) {
                uint64_t first = r->segments[i].phys & ~(uint64_t)(PAGE_SIZE - 1);
                uint64_t last = (r->segments[i].phys + r->segments[i].bytes - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                for (uint64_t at = first; at <= last; at += PAGE_SIZE) h.pfn[pages++] = at >> 12;
            }
            uint32_t hlen = (uint32_t)(offsetof(struct gpa_header, pfn) + pages * 8);
            h.d.type = VM_PKT_DATA_GPA_DIRECT;
            h.d.offset8 = (uint16_t)(hlen / 8);
            h.d.len8 = (uint16_t)((hlen + sizeof p + 7) / 8);
            h.d.flags = VMBUS_FLAG_COMPLETION_REQUESTED;
            h.d.trans_id = id + 1ull;
            h.range_count = 1;
            h.len = n * 512;
            h.offset = (uint32_t)(r->segments[0].phys & (PAGE_SIZE - 1));
            e = vmbus_send_raw(s->ch, &h, hlen, &p, sizeof p);
        }
        if (e != -N_ERR_NOT_READY || __atomic_load_n(&s->dead, __ATOMIC_ACQUIRE)) return e;
        sleep_until(clock_after(NS_PER_MS)); /* the ring is full: the host reads, and the room comes */
    }
}

static void scsi_submit(struct blkdev *b, struct blk_request *chain) {
    struct scsi_disk *d = b->priv;
    for (struct blk_request *r = chain, *next; r; r = next) {
        next = r->next;
        r->next = NULL;
        int id = slot_take(d->hba, r);
        if (id < 0) {
            blk_complete(r, -N_ERR_IO);
            continue;
        }
        int e = packet_send(d, r, (unsigned)id);
        struct blk_request *mine = e ? slot_claim(d->hba, (unsigned)id) : NULL;
        if (mine) blk_complete(mine, e);
    }
}

static const struct blk_ops scsi_ops = {.submit = scsi_submit};

/* ---- the controller's end ---- */
/* The channel closed (after which the host reads and writes nothing of ours), every request under
   way failed, the disks gone. Once. May sleep. */
static void controller_end(struct storvsc *s, const char *why, int reason) {
    uint64_t f = spin_lock(&s->lock);
    bool first = !__atomic_exchange_n(&s->dead, true, __ATOMIC_ACQ_REL);
    spin_unlock(&s->lock, f);
    if (!first) return;
    if (why) kprintf("storvsc: %s: its disks are gone\n", why);
    for (unsigned i = 0; i < s->ndisks; i++) __atomic_store_n(&s->disks[i]->blk.gone, true, __ATOMIC_RELEASE);
    vmbus_close(s->ch);
    for (unsigned i = 0; i < SLOTS; i++) {
        struct blk_request *r = slot_claim(s, i);
        if (r) blk_complete(r, -N_ERR_IO);
    }
    if (reason) hal_failed(s->hal, reason);
}

static void watchdog(void *arg) {
    struct storvsc *s = arg;
    while (!__atomic_load_n(&s->stopping, __ATOMIC_ACQUIRE) && !__atomic_load_n(&s->dead, __ATOMIC_ACQUIRE)) {
        sleep_until(clock_after(NS_PER_S));
        bool late = false;
        uint64_t f = spin_lock(&s->lock), now = clock_now_ns();
        for (unsigned i = 0; i < SLOTS; i++) late |= s->slots[i].request && s->slots[i].deadline <= now;
        spin_unlock(&s->lock, f);
        if (late) controller_end(s, "the host has not answered a request", -N_ERR_TIMEOUT);
    }
}

static void probe_lun(struct storvsc *s, struct hal_device *hal, uint8_t lun) {
    static int nscsi;
    char model[41];
    uint64_t sectors;
    mutex_lock(&s->control);
    int r = identify(s, lun, model, &sectors);
    mutex_unlock(&s->control);
    if (r || sectors > 0xFFFFFFFFull + 1) return; /* READ(10) names a block in 32 bits */
    struct scsi_disk *d = kzalloc(sizeof *d);
    if (!d) return;
    d->hba = s;
    d->lun = lun;
    ksnprintf(d->blk.name, sizeof d->blk.name, "scsi%d", nscsi++);
    strlcpy(d->blk.model, model, sizeof d->blk.model);
    d->blk.sectors = sectors;
    d->blk.max_sectors = s->max_sectors;
    d->blk.max_segments = MOST_PAGES;
    d->blk.segment_boundary = PAGE_SIZE;
    d->blk.has_cache = true;
    d->blk.ops = &scsi_ops;
    d->blk.priv = d;
    s->disks[s->ndisks++] = d;
    blk_register(hal, &d->blk);
}

static int storvsc_attach(struct hal_device *hal) {
    struct vmbus_channel *ch = vmbus_channel_of(hal);
    struct storvsc *s = kzalloc(sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    s->ch = ch;
    s->hal = hal;
    s->bounce_pa = pmm_alloc();
    if (!s->bounce_pa) {
        kfree(s);
        return -N_ERR_NO_MEMORY;
    }
    s->bounce = phys_to_virt(s->bounce_pa);
    if (vmbus_open(ch, 64 * 1024, 64 * 1024, storvsc_callback, s)) {
        pmm_free(s->bounce_pa);
        kfree(s);
        return -N_ERR_IO;
    }
    mutex_lock(&s->control);
    int r = negotiate(s);
    mutex_unlock(&s->control);
    if (r) {
        vmbus_close(ch);
        pmm_free(s->bounce_pa);
        kfree(s);
        return -N_ERR_IO;
    }
    hal->driver_data = s;
    kthread_create("storvsc", watchdog, s);
    for (int lun = 0; lun < LUNS; lun++) probe_lun(s, hal, (uint8_t)lun);
    return 0;
}

/* The channel is withdrawn (the host rescinds it): what is in the host's hands is failed. The disks
   stay listed, gone (blk.c never forgets one), and so does the controller they point to. */
static void storvsc_detach(struct hal_device *hal) {
    struct storvsc *s = hal->driver_data;
    __atomic_store_n(&s->stopping, true, __ATOMIC_RELEASE);
    controller_end(s, NULL, 0);
}

static const struct n_hal_rule storvsc_rules[] = {N_VMBUS_TYPE(scsi_guid.b)};
static const struct hal_driver storvsc_driver = {
    .name = "storvsc",
    .rules = storvsc_rules,
    .rule_count = ARRAY_SIZE(storvsc_rules),
    .attach = storvsc_attach,
    .detach = storvsc_detach,
};
HAL_DRIVER(storvsc_driver);
