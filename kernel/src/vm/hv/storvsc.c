/* Hyper-V synthetic SCSI controller (storvsc): SCSI commands carried over VMBus. Generation 2 VMs
   keep every disk and DVD drive here. Requests are synchronous, one at a time per controller from
   any CPU (its lock), through a bounce buffer that is handed to the host as a GPA-direct packet.
   The reply comes in the channel's callback, in the interrupt handler of the CPU VMBus signals;
   the request sleeps until then. */
#include "kernel.h"
#include "dev/blk.h"
#include "vm/hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/sched.h"

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

#define BOUNCE_PAGES 16 /* 64 KiB per request */

struct gpa_header {
    struct vmpacket_desc d;
    uint32_t reserved, range_count;
    uint32_t len, offset;
    uint64_t pfn[BOUNCE_PAGES];
} PACKED;

struct storvsc {
    struct vmbus_channel *ch;
    struct mutex lock; /* one request at a time: tid, reply and the bounce buffer */
    /* The callback (one at a time, in the VMBus interrupt) writes reply for the request numbered
       tid, puts its number in answered, counts replies up and wakes wq; the request reads replies
       before it looks at answered and sleeps only while replies still reads that (wq_wait_unless). */
    uint64_t tid, answered;
    uint64_t replies;
    struct wait_queue wq;
    struct vstor_packet reply;
    uint8_t *bounce;
    uint64_t bounce_pa;
    uint32_t max_sectors;
};

struct scsi_disk {
    struct blkdev blk;
    struct storvsc *hba;
    uint8_t lun;
};

static void storvsc_callback(struct vmbus_channel *ch) {
    struct storvsc *s = ch->priv;
    uint8_t pkt[256];
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0) continue;
        struct vmpacket_desc *d = (void *)pkt;
        /* bus change notices are ignored, and a late reply to a request given up on */
        if (d->type != VM_PKT_COMP || d->trans_id != __atomic_load_n(&s->tid, __ATOMIC_ACQUIRE)) continue;
        memset(&s->reply, 0, sizeof s->reply);
        memcpy(&s->reply, vmpacket_data(pkt), MIN(vmpacket_datalen(pkt), sizeof s->reply));
        __atomic_store_n(&s->answered, d->trans_id, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s->replies, 1, __ATOMIC_SEQ_CST);
        wq_wake_all(&s->wq);
    }
}

/* Until the reply to request tid has come, or the deadline: false when it did not. A late reply to
   the one before, whose callback read the old number, comes before this one's: the reply this one
   reads is its own. */
static bool reply_wait(struct storvsc *s, uint64_t tid, uint64_t deadline) {
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->replies, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s->answered, __ATOMIC_ACQUIRE) == tid) return true;
        if (clock_now_ns() >= deadline) return false;
        wq_wait_unless(&s->wq, &s->replies, seen, deadline);
    }
}

/* Send a packet (with the bounce buffer attached if len > 0) and wait for its completion. The
   controller's lock held. */
static int exchange(struct storvsc *s, struct vstor_packet *p, uint32_t len) {
    uint64_t tid = s->tid + 1;
    __atomic_store_n(&s->tid, tid, __ATOMIC_RELEASE); /* before the packet: its reply finds it */
    int r;
    if (len) {
        struct gpa_header h;
        memset(&h, 0, sizeof h);
        uint32_t pages = (uint32_t)(ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE);
        uint32_t hlen = (uint32_t)(offsetof(struct gpa_header, pfn) + pages * 8);
        h.d.type = VM_PKT_DATA_GPA_DIRECT;
        h.d.offset8 = (uint16_t)(hlen / 8);
        h.d.len8 = (uint16_t)((hlen + sizeof *p + 7) / 8);
        h.d.flags = VMBUS_FLAG_COMPLETION_REQUESTED;
        h.d.trans_id = s->tid;
        h.range_count = 1;
        h.len = len;
        for (uint32_t i = 0; i < pages; i++) h.pfn[i] = (s->bounce_pa >> 12) + i;
        r = vmbus_send_raw(s->ch, &h, hlen, p, sizeof *p);
    } else {
        r = vmbus_send(s->ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, p, sizeof *p, s->tid);
    }
    if (r < 0) return r;
    if (!reply_wait(s, tid, clock_after(10 * NS_PER_S))) {
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

/* Run one SCSI command against `lun`; data goes through the bounce buffer. */
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
    p.srb.timeout = 60;
    int r = exchange(s, &p, dir == DIR_NONE ? 0 : len);
    if (r) return r;
    if (s->reply.status || (s->reply.srb.srb_status & 0x3F) != SRB_STATUS_SUCCESS || s->reply.srb.scsi_status) return -N_ERR_IO;
    return 0;
}

/* Each command of up to max_sectors holds the controller's lock from filling the bounce buffer to
   emptying it; between commands another CPU's request may go first. */
static int rw(struct scsi_disk *d, uint64_t lba, uint32_t count, uint8_t *buf, bool write) {
    struct storvsc *s = d->hba;
    if (lba + count > d->blk.sectors || lba + count > 0xFFFFFFFFull) return -N_ERR_IO;
    while (count) {
        uint32_t n = MIN(count, s->max_sectors);
        uint8_t cdb[10] = {write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                           (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0};
        mutex_lock(&s->lock);
        if (write) memcpy(s->bounce, buf, n * 512);
        int r = scsi(s, d->lun, cdb, 10, write ? DIR_OUT : DIR_IN, n * 512);
        if (!r && !write) memcpy(buf, s->bounce, n * 512);
        mutex_unlock(&s->lock);
        if (r) return r;
        buf += n * 512;
        lba += n;
        count -= n;
        if (count && need_resched) schedule();
    }
    return 0;
}

static int blk_scsi_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, lba, n, buf, false); }
static int blk_scsi_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, lba, n, (uint8_t *)buf, true);
}

/* SYNCHRONIZE CACHE(10) (SBC): what was written reaches the virtual disk. */
static int blk_scsi_persist(struct blkdev *b) {
    struct scsi_disk *d = b->priv;
    uint8_t cdb[10] = {0x35};
    mutex_lock(&d->hba->lock);
    int r = scsi(d->hba, d->lun, cdb, 10, DIR_NONE, 0);
    mutex_unlock(&d->hba->lock);
    return r;
}

/* INQUIRY and READ CAPACITY(10) of one LUN, the controller's lock held: 0 with its model and size,
   or nonzero when it is no 512-byte disk. A disk found before may be in use on another CPU. */
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

static void probe_lun(struct storvsc *s, uint8_t lun) {
    static int nscsi;
    char model[41];
    uint64_t sectors;
    mutex_lock(&s->lock);
    int r = identify(s, lun, model, &sectors);
    mutex_unlock(&s->lock);
    if (r) return;
    struct scsi_disk *d = kzalloc(sizeof *d);
    if (!d) return;
    d->hba = s;
    d->lun = lun;
    ksnprintf(d->blk.name, sizeof d->blk.name, "scsi%d", nscsi++);
    strlcpy(d->blk.model, model, sizeof d->blk.model);
    d->blk.sectors = sectors;
    d->blk.read = blk_scsi_read;
    d->blk.write = blk_scsi_write;
    d->blk.persist = blk_scsi_persist;
    d->blk.priv = d;
    blk_register(&d->blk);
}

/* The protocol's opening (begin, version, properties, end), the controller's lock held: 0 when the
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
    uint32_t max_transfer = BOUNCE_PAGES * PAGE_SIZE;
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

static void storvsc_start(struct vmbus_channel *ch) {
    struct storvsc *s = kzalloc(sizeof *s);
    if (!s) return;
    s->ch = ch;
    s->bounce_pa = pmm_alloc_contig(BOUNCE_PAGES, 64);
    if (!s->bounce_pa) return;
    s->bounce = phys_to_virt(s->bounce_pa);
    if (vmbus_open(ch, 64 * 1024, 64 * 1024, storvsc_callback, s)) return;
    mutex_lock(&s->lock);
    int r = negotiate(s);
    mutex_unlock(&s->lock);
    if (r) return;
    for (int lun = 0; lun < 64; lun++) probe_lun(s, (uint8_t)lun);
}

void storvsc_init(void) {
    if (!vmbus_ready()) return;
    struct vmbus_channel *ch;
    for (int i = 0; (ch = vmbus_find(&scsi_guid, i)) != NULL; i++) storvsc_start(ch);
}
