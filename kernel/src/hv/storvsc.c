/* Hyper-V synthetic SCSI controller (storvsc): SCSI commands carried over VMBus. Generation 2 VMs
   keep every disk and DVD drive here. Requests are synchronous, one at a time, through a bounce
   buffer that is handed to the host as a GPA-direct packet; like the ATA driver, a transfer runs
   to completion before the caller continues. */
#include "kernel.h"
#include "dev/blk.h"
#include "hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"

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
    volatile bool done;
    uint64_t tid;
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
        if (d->type != VM_PKT_COMP || d->trans_id != s->tid) continue; /* bus change notices are ignored */
        memset(&s->reply, 0, sizeof s->reply);
        memcpy(&s->reply, vmpacket_data(pkt), MIN(vmpacket_datalen(pkt), sizeof s->reply));
        s->done = true;
    }
}

/* Send a packet (with the bounce buffer attached if len > 0) and wait for its completion. */
static int exchange(struct storvsc *s, struct vstor_packet *p, uint32_t len) {
    s->done = false;
    s->tid++;
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
    if (!vmbus_wait(&s->done, 10000)) {
        kprintf("storvsc: request timed out\n");
        return -ETIMEDOUT;
    }
    return 0;
}

static int control(struct storvsc *s, uint32_t op, struct vstor_packet *p) {
    p->op = op;
    p->flags = VSTOR_REQUEST_COMPLETION;
    int r = exchange(s, p, 0);
    if (r) return r;
    return s->reply.status ? -EIO : 0;
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
    if (s->reply.status || (s->reply.srb.srb_status & 0x3F) != SRB_STATUS_SUCCESS || s->reply.srb.scsi_status) return -EIO;
    return 0;
}

static int rw(struct scsi_disk *d, uint64_t lba, uint32_t count, uint8_t *buf, bool write) {
    struct storvsc *s = d->hba;
    if (lba + count > d->blk.sectors || lba + count > 0xFFFFFFFFull) return -EIO;
    while (count) {
        uint32_t n = MIN(count, s->max_sectors);
        uint8_t cdb[10] = {write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                           (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0};
        if (write) memcpy(s->bounce, buf, n * 512);
        int r = scsi(s, d->lun, cdb, 10, write ? DIR_OUT : DIR_IN, n * 512);
        if (r) return r;
        if (!write) memcpy(buf, s->bounce, n * 512);
        buf += n * 512;
        lba += n;
        count -= n;
    }
    if (write) { /* SYNCHRONIZE CACHE: make sure it reaches the virtual disk before we report success */
        uint8_t cdb[10] = {0x35};
        return scsi(s, d->lun, cdb, 10, DIR_NONE, 0);
    }
    return 0;
}

static int blk_scsi_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, lba, n, buf, false); }
static int blk_scsi_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, lba, n, (uint8_t *)buf, true);
}

static void probe_lun(struct storvsc *s, uint8_t lun) {
    static int nscsi;
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    if (scsi(s, lun, inq, 6, DIR_IN, 36)) return;
    uint8_t type = s->bounce[0];
    if ((type & 0xE0) || (type & 0x1F) != 0) return; /* not present, or not a disk (e.g. the DVD drive) */
    char model[41];
    memcpy(model, s->bounce + 8, 8);
    model[8] = ' ';
    memcpy(model + 9, s->bounce + 16, 16);
    model[25] = 0;
    for (int i = 24; i >= 0 && (model[i] == ' ' || !model[i]); i--) model[i] = 0;
    uint8_t cap[10] = {0x25};
    if (scsi(s, lun, cap, 10, DIR_IN, 8)) return;
    uint8_t *c = s->bounce;
    uint64_t last = (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
    uint32_t bsize = (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7];
    if (bsize != 512) {
        kprintf("storvsc: LUN %u has %u-byte sectors, skipped\n", lun, bsize);
        return;
    }
    struct scsi_disk *d = kzalloc(sizeof *d);
    d->hba = s;
    d->lun = lun;
    ksnprintf(d->blk.name, sizeof d->blk.name, "scsi%d", nscsi++);
    strlcpy(d->blk.model, model, sizeof d->blk.model);
    d->blk.sectors = last + 1;
    d->blk.read = blk_scsi_read;
    d->blk.write = blk_scsi_write;
    d->blk.priv = d;
    blk_register(&d->blk);
}

static void storvsc_start(struct vmbus_channel *ch) {
    struct storvsc *s = kzalloc(sizeof *s);
    s->ch = ch;
    s->bounce_pa = pmm_alloc_contig(BOUNCE_PAGES);
    if (!s->bounce_pa) return;
    s->bounce = phys_to_virt(s->bounce_pa);
    if (vmbus_open(ch, 64 * 1024, 64 * 1024, storvsc_callback, s)) return;

    struct vstor_packet p;
    memset(&p, 0, sizeof p);
    if (control(s, VSTOR_BEGIN_INIT, &p)) {
        kprintf("storvsc: begin-initialization failed\n");
        return;
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
        return;
    }
    memset(&p, 0, sizeof p);
    uint32_t max_transfer = BOUNCE_PAGES * PAGE_SIZE;
    if (control(s, VSTOR_QUERY_PROPERTIES, &p) == 0 && s->reply.props.max_transfer)
        max_transfer = MIN(max_transfer, s->reply.props.max_transfer);
    s->max_sectors = max_transfer / 512;
    memset(&p, 0, sizeof p);
    if (control(s, VSTOR_END_INIT, &p)) {
        kprintf("storvsc: end-initialization failed\n");
        return;
    }
    for (int lun = 0; lun < 64; lun++) probe_lun(s, (uint8_t)lun);
}

void storvsc_init(void) {
    if (!vmbus_ready()) return;
    struct vmbus_channel *ch;
    for (int i = 0; (ch = vmbus_find(&scsi_guid, i)) != NULL; i++) storvsc_start(ch);
}
