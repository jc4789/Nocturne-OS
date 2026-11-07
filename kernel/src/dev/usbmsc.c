/* USB mass storage: the Bulk-Only Transport (USB MSC BOT 1.0) carrying SCSI commands, the set
 * the USB MSC bootability spec says every device answers plus READ CAPACITY(16) and the 16-byte
 * READ/WRITE for large disks. Each logical unit with a medium becomes a block device whose requests
 * run one at a time on a thread of its own (blk_serial_start) and one command at a time on the
 * device under its lock; the block device outlives an unplug and then fails. */
#include "kernel.h"
#include "dev/blk.h"
#include "dev/usb.h"
#include "io/io.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define CBW_SIG 0x43425355u /* "USBC" */
#define CSW_SIG 0x53425355u /* "USBS" */
#define XFER 0x10000         /* bytes per command: usb_bulk's limit */
#define MAX_LUNS 16      /* Get Max LUN says 0 to 15 (BOT 3.2) */
#define BOT_FAILED (-1)      /* the command failed (CHECK CONDITION): sense data waits */
#define BOT_BROKEN (-2)      /* the transport broke; reset recovery ran */
/* How long a command's data and status may take: a medium error retried inside the device, or a
   unit that spins up for START STOP UNIT (whose status comes once it has started). */
#define CMD_MS 10000
#define START_MS 30000

/* A storage function. lock: one command at a time, and dev, which detach clears under it, so no
   command runs on an unplugged device, and stopped, which suspend sets under it. */
struct msc {
    struct mutex lock;
    struct usb_device *dev; /* NULL once unplugged */
    bool stopped;           /* 電源断の前に止めた：もう命令を出さない */
    struct usb_interface *node;
    uint8_t intf, in, out;
    uint32_t tag;
    uint8_t sense_key, asc, ascq;
    uint8_t *bounce; /* a request whose memory is in several pieces goes through this */
    struct lun *luns; /* its block devices */
};

struct lun {
    struct blkdev blk;
    struct msc *m;
    struct lun *next;
    uint8_t lun;
    uint32_t block; /* bytes per logical block */
    uint64_t blocks;
};

static unsigned disks; /* atomic: the names given */

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be(uint8_t *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
}

static bool gone(const struct usb_device *d) { return !d || __atomic_load_n(&d->removed, __ATOMIC_ACQUIRE); }

/* BOT 5.3.4: Bulk-Only Mass Storage Reset, then clear both bulk endpoints' halts. */
static void reset_recovery(struct msc *m) {
    struct usb_device *d = m->dev;
    if (gone(d)) return;
    usb_control(d, 0x21, 0xFF, 0, m->intf, NULL, 0);
    usb_clear_halt(d, m->in);
    usb_clear_halt(d, m->out);
}

/* One command: CBW, data stage, CSW (BOT 5.3, 6.7). Returns the bytes the device moved. */
static int bot(struct msc *m, unsigned lun, const uint8_t *cdb, unsigned cdb_len, void *data, unsigned len, bool in,
               unsigned timeout) {
    struct usb_device *d = m->dev;
    if (gone(d)) return BOT_BROKEN;
    uint8_t cbw[31] = {0}, csw[13];
    uint32_t tag = ++m->tag;
    put32(cbw, CBW_SIG);
    put32(cbw + 4, tag);
    put32(cbw + 8, len);
    cbw[12] = in && len ? 0x80 : 0;
    cbw[13] = (uint8_t)lun;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    unsigned moved = 0, data_moved = 0;
    if (usb_bulk(d, m->out, cbw, 31, 5000, &moved) || moved != 31) {
        reset_recovery(m);
        return BOT_BROKEN;
    }
    if (len) {
        uint8_t pipe = in ? m->in : m->out;
        /* a stall ends the data stage early (cases 5, 11): what came before it is kept (6.7.2), the CSW follows */
        int r = usb_bulk(d, pipe, data, len, timeout, &data_moved);
        if (r == USB_STALL) {
            usb_clear_halt(d, pipe);
        } else if (r < 0) {
            reset_recovery(m);
            return BOT_BROKEN;
        }
    }
    int r = usb_bulk(d, m->in, csw, 13, timeout, &moved);
    if (r < 0 && r != USB_TIMEOUT) { /* Figure 2, a stall or a bulk error: clear the halt and read the CSW once more */
        usb_clear_halt(d, m->in);
        r = usb_bulk(d, m->in, csw, 13, timeout, &moved);
    }
    /* 6.3: a CSW is valid when it is 13 bytes with the signature and our tag, and meaningful when
     * its status is 0 or 1 with a residue within the request; a phase error (status 2) and any
     * other are a Reset Recovery (6.5). What is relevant of the data is the request less the
     * residue (6.7.2), of which only what moved is there. */
    bool whole = !r && moved == 13;
    uint32_t residue = whole ? get32(csw + 8) : 0;
    if (!whole || get32(csw) != CSW_SIG || get32(csw + 4) != tag || csw[12] > 1 || residue > len) {
        reset_recovery(m);
        return BOT_BROKEN;
    }
    if (csw[12] == 1) return BOT_FAILED;
    return (int)MIN(len - residue, data_moved);
}

/* A command, and on CHECK CONDITION the sense data right away (any other command loses it). */
static int scsi_waiting(struct msc *m, unsigned lun, const uint8_t *cdb, unsigned cdb_len, void *data, unsigned len,
                        bool in, unsigned timeout) {
    int r = bot(m, lun, cdb, cdb_len, data, len, in, timeout);
    if (r != BOT_FAILED) return r;
    /* 252 bytes, and the fixed format: the command clears what it does not return (SPC, Seagate
       manual 3.37, 2.4.1) */
    uint8_t sense[252] = {0}, rs[6] = {0x03, 0, 0, 0, sizeof sense, 0};
    m->sense_key = m->asc = m->ascq = 0;
    if (bot(m, lun, rs, 6, sense, sizeof sense, true, 5000) >= 14 && ((sense[0] & 0x7F) == 0x70 || (sense[0] & 0x7F) == 0x71)) {
        m->sense_key = sense[2] & 15;
        m->asc = sense[12];
        m->ascq = sense[13];
    }
    return BOT_FAILED;
}

static int scsi(struct msc *m, unsigned lun, const uint8_t *cdb, unsigned cdb_len, void *data, unsigned len, bool in) {
    return scsi_waiting(m, lun, cdb, cdb_len, data, len, in, CMD_MS);
}

/* Is the unit ready (TEST UNIT READY)? Each answer says what to wait for (SPC sense data):
   a unit attention (power on, reset, medium changed) is reported once and cleared by the
   sense data that reported it, so the unit is asked again at once; a unit not ready but becoming
   so, or wanting its initializing command, gets START STOP UNIT with IMMED clear, whose status
   comes once the unit has started (SBC), and is asked once more. No medium, a transport that
   breaks twice, or a unit still not ready after its start is false. The attentions a unit has
   to report end; a unit that keeps reporting them is let go at the start's deadline. */
static bool unit_ready(struct msc *m, unsigned lun) {
    static const uint8_t tur[6] = {0}, start[6] = {0x1B, 0, 0, 0, 1, 0};
    bool started = false, broke = false;
    for (uint64_t deadline = clock_after(START_MS * NS_PER_MS); clock_now_ns() < deadline && !gone(m->dev);) {
        int r = scsi(m, lun, tur, 6, NULL, 0, false);
        if (r == 0) return true;
        if (r == BOT_BROKEN) { /* reset recovery ran: once more */
            if (broke) return false;
            broke = true;
            continue;
        }
        if (r != BOT_FAILED) return false;
        if (m->sense_key == 6) continue;
        if (m->sense_key != 2 || m->asc != 0x04 || started) return false; /* 3A: no medium */
        started = true;
        if (scsi_waiting(m, lun, start, 6, NULL, 0, false, START_MS) < 0 && m->sense_key != 6) return false;
    }
    return false;
}

static bool read_capacity(struct msc *m, unsigned lun, uint64_t *blocks, uint32_t *block) {
    uint8_t cdb[16] = {0x25}, cap[32];
    for (int tries = 0; tries < 3; tries++) {
        if (scsi(m, lun, cdb, 10, cap, 8, true) != 8) continue;
        uint32_t last = be32(cap);
        *block = be32(cap + 4);
        *blocks = (uint64_t)last + 1;
        if (last != 0xFFFFFFFFu) return true;
        memset(cdb, 0, sizeof cdb); /* more than 2^32 blocks: SERVICE ACTION IN, READ CAPACITY(16) */
        cdb[0] = 0x9E;
        cdb[1] = 0x10;
        cdb[13] = 32;
        if (scsi(m, lun, cdb, 16, cap, 32, true) < 12) return false;
        *blocks = ((uint64_t)be32(cap) << 32 | be32(cap + 4)) + 1;
        *block = be32(cap + 8);
        return true;
    }
    return false;
}

/* `n` whole logical blocks from `first`, in one command. */
static int blocks_io(struct lun *l, bool write, uint64_t first, unsigned n, uint8_t *data) {
    struct msc *m = l->m;
    unsigned bytes = n * l->block;
    uint8_t cdb[16] = {0};
    unsigned len;
    if (first + n <= 0xFFFFFFFFull && n <= 0xFFFF) {
        cdb[0] = write ? 0x2A : 0x28;
        put_be(cdb + 2, first, 4);
        put_be(cdb + 7, n, 2);
        len = 10;
    } else {
        cdb[0] = write ? 0x8A : 0x88;
        put_be(cdb + 2, first, 8);
        put_be(cdb + 10, n, 4);
        len = 16;
    }
    for (int tries = 0; tries < 3 && m->dev; tries++) {
        int r = scsi(m, l->lun, cdb, len, data, bytes, !write);
        if (r == (int)bytes) return 0;
        if (r == BOT_FAILED && m->sense_key != 6) {
            kprintf("usbmsc: %s %s of block %lu failed (sense %x/%02x/%02x)\n", l->blk.name, write ? "write" : "read",
                    first, m->sense_key, m->asc, m->ascq);
            if (m->sense_key == 7 || m->sense_key == 5) break; /* write protected, illegal request */
        }
    }
    return -N_ERR_IO;
}

/* A read or write of the request's sectors: its memory straight when it is one piece, else through the
   bounce buffer (a request is at most XFER bytes: max_sectors). */
static int rw(struct lun *l, const struct blk_request *r) {
    struct msc *m = l->m;
    bool write = r->op == BLK_WRITE;
    unsigned per = l->block / 512;
    uint8_t *data;
    if (r->segment_count == 1) {
        data = phys_to_virt(r->segments[0].phys);
    } else {
        if (!m->bounce && !(m->bounce = kmalloc(XFER))) return -N_ERR_NO_MEMORY;
        data = m->bounce;
        for (uint32_t i = 0, at = 0; write && i < r->segment_count; at += r->segments[i++].bytes)
            memcpy(data + at, phys_to_virt(r->segments[i].phys), r->segments[i].bytes);
    }
    int status = blocks_io(l, write, r->lba / per, r->sectors / per, data);
    for (uint32_t i = 0, at = 0; !status && !write && r->segment_count > 1 && i < r->segment_count; at += r->segments[i++].bytes)
        memcpy(phys_to_virt(r->segments[i].phys), data + at, r->segments[i].bytes);
    return status;
}

/* SYNCHRONIZE CACHE(10) over the whole unit (SBC). A unit without a write cache may refuse it
   (ILLEGAL REQUEST): then nothing was waiting. */
static int flush(struct lun *l) {
    static const uint8_t cdb[10] = {0x35};
    struct msc *m = l->m;
    /* a unit attention (a reset, the power) or a moment's trouble is tried again, like a read */
    for (int tries = 0; tries < 3 && m->dev; tries++) {
        int r = scsi(m, l->lun, cdb, 10, NULL, 0, false);
        if (r >= 0 || (r == BOT_FAILED && m->sense_key == 5)) return 0; /* 5: no cache to flush */
        if (r == BOT_FAILED && m->sense_key != 6)
            kprintf("usbmsc: %s flush failed (sense %x/%02x/%02x)\n", l->blk.name, m->sense_key, m->asc, m->ascq);
    }
    return -N_ERR_IO;
}

/* Every request of the unit, on its serial thread: one command at a time on the device. */
static int msc_transfer(struct blkdev *b, const struct blk_request *r) {
    struct lun *l = b->priv;
    struct msc *m = l->m;
    mutex_lock(&m->lock);
    int status = !m->dev || m->stopped ? -N_ERR_IO : r->op == BLK_PERSIST ? flush(l) : rw(l, r);
    mutex_unlock(&m->lock);
    return status;
}

static void probe_lun(struct msc *m, unsigned lun) {
    uint8_t inq[36] = {0}, cdb[6] = {0x12, 0, 0, 0, sizeof inq, 0};
    int r = scsi(m, lun, cdb, 6, inq, sizeof inq, true);
    if (r < 5) return;
    unsigned type = inq[0] & 0x1F, qualifier = inq[0] >> 5;
    /* direct access (SBC), optical memory and RBC hold block filesystems; CD-ROMs do not */
    if (qualifier || (type != 0x00 && type != 0x07 && type != 0x0E)) {
        kprintf("usbmsc: slot %u unit %u is a type %02x device, skipped\n", m->dev->slot, lun, type);
        return;
    }
    char model[41];
    unsigned n = 0;
    for (int i = 8; i < 32 && n < sizeof model - 1; i++) {
        char c = inq[i] >= 0x20 && inq[i] < 0x7F ? (char)inq[i] : ' ';
        if (i == 16 && n && model[n - 1] != ' ') model[n++] = ' ';
        if (c == ' ' && (!n || model[n - 1] == ' ')) continue;
        model[n++] = c;
    }
    while (n && model[n - 1] == ' ') n--;
    model[n] = 0;
    if (!lun && m->dev) io_name(&m->dev->hal, model);
    if (!unit_ready(m, lun)) {
        if (m->dev) kprintf("usbmsc: %s (unit %u) has no medium or is not ready\n", model, lun);
        return;
    }
    uint64_t blocks;
    uint32_t block;
    if (!read_capacity(m, lun, &blocks, &block)) {
        kprintf("usbmsc: %s capacity unreadable\n", model);
        return;
    }
    if (block < 512 || (block & 511) || block > XFER || !blocks) {
        kprintf("usbmsc: %s has %u-byte blocks, not supported\n", model, block);
        return;
    }
    if (block > 512 && !m->bounce && !(m->bounce = kmalloc(XFER))) return;
    struct lun *l = kzalloc(sizeof *l);
    if (!l) return;
    l->m = m;
    l->lun = (uint8_t)lun;
    l->block = block;
    l->blocks = blocks;
    ksnprintf(l->blk.name, sizeof l->blk.name, "usb%u", __atomic_fetch_add(&disks, 1, __ATOMIC_RELAXED));
    strlcpy(l->blk.model, model, sizeof l->blk.model);
    l->blk.sectors = blocks * (block / 512);
    l->blk.block = block;
    l->blk.max_sectors = XFER / 512;
    l->blk.has_cache = true;
    l->blk.priv = l;
    l->next = m->luns;
    m->luns = l;
    blk_serial_start(&l->blk, msc_transfer);
    blk_register(&m->node->hal, &l->blk);
}

/* An interface of the mass storage class with a bulk pair: the descriptors from its own to the next
   interface's. */
static int msc_attach(struct io_device *hal) {
    struct usb_interface *node = usb_interface_of(hal);
    struct usb_device *d = node->dev;
    const uint8_t *cfg = d->config;
    unsigned total = d->config_bytes;
    struct usb_ep_info eps[2], e;
    bool have_in = false, have_out = false;
    for (unsigned off = node->offset + node->desc->length; off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total;
         off += cfg[off]) {
        if (cfg[off + 1] == USB_DESC_INTERFACE) break; /* BOT 4: the first bulk IN and OUT of the interface */
        if (usb_endpoint_info(cfg, total, off, &e) && (e.attributes & 3) == 2) {
            if ((e.address & 0x80) && !have_in) eps[0] = e, have_in = true;
            if (!(e.address & 0x80) && !have_out) eps[1] = e, have_out = true;
        }
    }
    if (!have_in || !have_out) {
        kprintf("usbmsc: slot %u has no usable bulk endpoints\n", d->slot);
        return -N_ERR_UNSUPPORTED;
    }
    struct msc *m = kzalloc(sizeof *m);
    if (!m) return -N_ERR_NO_MEMORY;
    m->dev = d;
    m->node = node;
    m->intf = node->desc->number;
    m->in = eps[0].address;
    m->out = eps[1].address;
    hal->driver_data = m;
    /* BOT 3.2: Get Max LUN; a device with one unit may stall it */
    uint8_t max = 0;
    if (usb_control(d, 0xA1, 0xFE, 0, m->intf, &max, 1) < 1) max = 0;
    if (max >= MAX_LUNS) max = MAX_LUNS - 1;
    kprintf("usbmsc: slot %u bulk-only storage, %u unit%s\n", d->slot, max + 1u, max ? "s" : "");
    mutex_lock(&m->lock);
    for (unsigned lun = 0; lun <= max && m->dev && !io_gone(hal); lun++) probe_lun(m, lun);
    mutex_unlock(&m->lock);
    return 0;
}

/* The block devices stay registered (a mounted filesystem may still hold them); they fail from
 * now on, and their serial threads end once what they hold is failed. The command running ends
 * first: usb_remove ended its transfers. Their memory stays, for whoever holds the devices. */
static void msc_detach(struct io_device *hal) {
    struct msc *m = hal->driver_data;
    mutex_lock(&m->lock);
    m->dev = NULL;
    for (struct lun *l = m->luns; l; l = l->next) __atomic_store_n(&l->blk.gone, true, __ATOMIC_RELEASE);
    mutex_unlock(&m->lock);
    for (struct lun *l = m->luns; l; l = l->next) blk_serial_stop(&l->blk);
    hal->driver_data = NULL;
}

/* 戻せない（IO_AFTER_SLEEP_LOST）：休止の写しから戻る前に起動のカーネルが制御器を reset し、眠りの間に port の電源
   （VBUS）が切れれば装置は reset される。どちらでもディスクは新しい装置として戻り、その上のボリュームは失われる。だから
   suspend に来るのは電源断だけで、進行中の命令の終わりを待ち、その後の要求を断る。 */
static bool msc_suspend(struct io_device *hal, enum io_power_target target) {
    ASSERT(target == IO_POWER_OFF);
    struct msc *m = hal->driver_data;
    mutex_lock(&m->lock);
    m->stopped = true;
    mutex_unlock(&m->lock);
    return true;
}

/* Bulk-only (protocol 50h) with a command set that speaks SCSI (MSC Overview Table 1): not reported
   00h, RBC 01h, MMC-5 02h, SCSI transparent 06h. */
#define MSC_BULK_ONLY(subclass) \
    N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_USB_INTERFACE), N_HAL_NUMBER(N_PROP_USB_CLASS, N_USB_CLASS_CODE(8, subclass, 0x50)))
static const struct n_hal_rule msc_rules[] = {MSC_BULK_ONLY(0x00), MSC_BULK_ONLY(0x01), MSC_BULK_ONLY(0x02),
                                              MSC_BULK_ONLY(0x06)};
static const struct io_driver msc_driver = {
    .name = "usbmsc",
    .rules = msc_rules,
    .rule_count = ARRAY_SIZE(msc_rules),
    .attach = msc_attach,
    .detach = msc_detach,
    .suspend = msc_suspend,
    .after_sleep = IO_AFTER_SLEEP_LOST,
};
IO_DRIVER(msc_driver);
