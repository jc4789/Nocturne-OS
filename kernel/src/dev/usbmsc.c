/* USB mass storage: the Bulk-Only Transport (USB MSC BOT 1.0) carrying SCSI commands, the set
 * the USB MSC bootability spec says every device answers plus READ CAPACITY(16) and the 16-byte
 * READ/WRITE for large disks. Each logical unit with a medium becomes a block device. Its reads
 * and writes run on the usb thread (usb_run); the block device outlives an unplug and then fails. */
#include "kernel.h"
#include "dev/blk.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "sys/sched.h"

#define CBW_SIG 0x43425355u /* "USBC" */
#define CSW_SIG 0x53425355u /* "USBS" */
#define XFER 0x10000         /* bytes per command: usb_bulk's limit */
#define MAX_LUNS 8
#define BOT_FAILED (-1)      /* the command failed (CHECK CONDITION): sense data waits */
#define BOT_BROKEN (-2)      /* the transport broke; reset recovery ran */

struct msc {
    struct usb_device *dev; /* NULL once unplugged */
    uint8_t intf, in, out;
    uint32_t tag;
    uint8_t sense_key, asc, ascq;
    uint8_t *bounce; /* for logical blocks larger than the 512-byte sectors */
};

struct lun {
    struct blkdev blk;
    struct msc *m;
    uint8_t lun;
    uint32_t block; /* bytes per logical block */
    uint64_t blocks;
};

static unsigned disks;

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be(uint8_t *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
}

bool usbmsc_claims(const struct usb_interface_desc *intf) {
    return intf->cls == 8 && intf->protocol == 0x50 && intf->alternate == 0;
}

/* BOT 5.3.4: Bulk-Only Mass Storage Reset, then clear both bulk endpoints' halts. */
static void reset_recovery(struct msc *m) {
    struct usb_device *d = m->dev;
    if (!d || d->removed) return;
    usb_control(d, 0x21, 0xFF, 0, m->intf, NULL, 0);
    usb_clear_halt(d, m->in);
    usb_clear_halt(d, m->out);
}

/* One command: CBW, data stage, CSW (BOT 5.3, 6.7). Returns the bytes the device moved. */
static int bot(struct msc *m, unsigned lun, const uint8_t *cdb, unsigned cdb_len, void *data, unsigned len, bool in,
               unsigned timeout) {
    struct usb_device *d = m->dev;
    if (!d || d->removed) return BOT_BROKEN;
    uint8_t cbw[31] = {0}, csw[13];
    uint32_t tag = ++m->tag;
    put32(cbw, CBW_SIG);
    put32(cbw + 4, tag);
    put32(cbw + 8, len);
    cbw[12] = in && len ? 0x80 : 0;
    cbw[13] = (uint8_t)lun;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    if (usb_bulk(d, m->out, cbw, 31, 5000) != 31) {
        reset_recovery(m);
        return BOT_BROKEN;
    }
    if (len) {
        uint8_t pipe = in ? m->in : m->out;
        int r = usb_bulk(d, pipe, data, len, timeout);
        if (r == USB_STALL) { /* the device ends the data stage early (cases 5, 11): the CSW follows */
            usb_clear_halt(d, pipe);
        } else if (r < 0) {
            reset_recovery(m);
            return BOT_BROKEN;
        }
    }
    int r = usb_bulk(d, m->in, csw, 13, timeout);
    if (r == USB_STALL) { /* Figure 2: clear the halt and read the CSW once more */
        usb_clear_halt(d, m->in);
        r = usb_bulk(d, m->in, csw, 13, timeout);
    }
    /* 6.3: a CSW is valid when it is 13 bytes with the signature and our tag; a phase error
     * (status 2) or a residue beyond the request means the two sides disagree */
    uint32_t residue = r == 13 ? get32(csw + 8) : 0;
    if (r != 13 || get32(csw) != CSW_SIG || get32(csw + 4) != tag || csw[12] > 1 || residue > len) {
        reset_recovery(m);
        return BOT_BROKEN;
    }
    if (csw[12] == 1) return BOT_FAILED;
    return (int)(len - residue);
}

/* A command, and on CHECK CONDITION the sense data right away (any other command loses it). */
static int scsi(struct msc *m, unsigned lun, const uint8_t *cdb, unsigned cdb_len, void *data, unsigned len, bool in) {
    int r = bot(m, lun, cdb, cdb_len, data, len, in, 10000);
    if (r != BOT_FAILED) return r;
    uint8_t sense[18] = {0}, rs[6] = {0x03, 0, 0, 0, sizeof sense, 0};
    m->sense_key = m->asc = m->ascq = 0;
    if (bot(m, lun, rs, 6, sense, sizeof sense, true, 5000) >= 14) {
        m->sense_key = sense[2] & 15;
        m->asc = sense[12];
        m->ascq = sense[13];
    }
    return BOT_FAILED;
}

/* Wait for the unit to become ready (TEST UNIT READY). False when it has no medium or fails. */
static bool unit_ready(struct msc *m, unsigned lun) {
    static const uint8_t tur[6] = {0}, start[6] = {0x1B, 0, 0, 0, 1, 0};
    bool started = false;
    for (int tries = 0; tries < 40; tries++) {
        int r = scsi(m, lun, tur, 6, NULL, 0, false);
        if (r == 0) return true;
        if (r == BOT_BROKEN && !m->dev) return false;
        if (r == BOT_FAILED) {
            if (m->sense_key == 6) continue; /* unit attention: power on, reset, medium changed */
            if (m->sense_key == 2 && m->asc == 0x3A) return false; /* no medium */
            if (m->sense_key == 2 && m->asc == 0x04 && m->ascq == 0x02 && !started) {
                scsi(m, lun, start, 6, NULL, 0, false); /* "initializing command required": spin up */
                started = true;
            }
        }
        sleep_ms(250);
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
static int transfer(struct lun *l, bool write, uint64_t first, unsigned n, void *data) {
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
    return -1;
}

struct io {
    struct lun *l;
    bool write;
    uint64_t lba;
    uint32_t count;
    uint8_t *buf;
    int result;
};

/* 512-byte sectors over the unit's logical blocks. Larger blocks go through the bounce buffer;
 * a write that covers part of one reads it first. */
static void do_io(void *arg) {
    struct io *io = arg;
    struct lun *l = io->l;
    struct msc *m = l->m;
    io->result = -1;
    if (!m->dev || io->lba + io->count > l->blk.sectors) return;
    unsigned per = l->block / 512, most = XFER / l->block;
    uint64_t lba = io->lba;
    uint32_t left = io->count;
    uint8_t *p = io->buf;
    while (left) {
        if (per == 1) {
            unsigned n = left < most ? left : most;
            if (transfer(l, io->write, lba, n, p) < 0) return;
            lba += n;
            left -= n;
            p += n * 512u;
            continue;
        }
        uint64_t first = lba / per;
        unsigned off = (unsigned)(lba % per) * 512, want = left * 512u;
        unsigned n = (off + want + l->block - 1) / l->block;
        if (n > most) n = most;
        unsigned take = n * l->block - off;
        if (take > want) take = want;
        if (io->write) {
            if ((off || take < n * l->block) && transfer(l, false, first, n, m->bounce) < 0) return;
            memcpy(m->bounce + off, p, take);
            if (transfer(l, true, first, n, m->bounce) < 0) return;
        } else {
            if (transfer(l, false, first, n, m->bounce) < 0) return;
            memcpy(p, m->bounce + off, take);
        }
        lba += take / 512;
        left -= take / 512;
        p += take;
    }
    io->result = 0;
}

static int msc_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf) {
    struct io io = {b->priv, false, lba, count, buf, -1};
    usb_run(do_io, &io);
    return io.result;
}

static int msc_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf) {
    struct io io = {b->priv, true, lba, count, (uint8_t *)buf, -1};
    usb_run(do_io, &io);
    return io.result;
}

/* SYNCHRONIZE CACHE(10) over the whole unit (SBC). A unit without a write cache may refuse it
   (ILLEGAL REQUEST): then nothing was waiting. */
static void do_flush(void *arg) {
    struct io *io = arg;
    static const uint8_t cdb[10] = {0x35};
    struct msc *m = io->l->m;
    io->result = -1;
    if (!m->dev) return;
    int r = scsi(m, io->l->lun, cdb, 10, NULL, 0, false);
    io->result = r >= 0 || (r == BOT_FAILED && m->sense_key == 5) ? 0 : -1;
}

static int msc_flush(struct blkdev *b) {
    struct io io = {b->priv, false, 0, 0, NULL, -1};
    usb_run(do_flush, &io);
    return io.result;
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
    ksnprintf(l->blk.name, sizeof l->blk.name, "usb%u", disks++);
    strlcpy(l->blk.model, model, sizeof l->blk.model);
    l->blk.sectors = blocks * (block / 512);
    l->blk.read = msc_read;
    l->blk.write = msc_write;
    l->blk.flush = msc_flush;
    l->blk.priv = l;
    blk_register(&l->blk);
}

void usbmsc_attach(struct usb_device *d, const uint8_t *cfg, unsigned total) {
    const struct usb_interface_desc *intf = NULL;
    struct usb_ep_info eps[2], e;
    bool have_in = false, have_out = false;
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total; off += cfg[off]) {
        unsigned type = cfg[off + 1];
        if (type == USB_DESC_INTERFACE && cfg[off] >= 9) {
            if (intf) break; /* BOT 4: the first bulk IN and OUT of the interface */
            const struct usb_interface_desc *i = (const void *)(cfg + off);
            if (usbmsc_claims(i)) intf = i;
        } else if (intf && usb_endpoint_info(cfg, total, off, &e) && (e.attributes & 3) == 2) {
            if ((e.address & 0x80) && !have_in) eps[0] = e, have_in = true;
            if (!(e.address & 0x80) && !have_out) eps[1] = e, have_out = true;
        }
    }
    if (!intf) return;
    if (!have_in || !have_out || !usb_open_endpoints(d, eps, 2)) {
        kprintf("usbmsc: slot %u has no usable bulk endpoints\n", d->slot);
        return;
    }
    struct msc *m = kzalloc(sizeof *m);
    if (!m) return;
    m->dev = d;
    m->intf = intf->number;
    m->in = eps[0].address;
    m->out = eps[1].address;
    d->msc = m;
    /* BOT 3.2: Get Max LUN; a device with one unit may stall it */
    uint8_t max = 0;
    if (usb_control(d, 0xA1, 0xFE, 0, m->intf, &max, 1) < 1) max = 0;
    if (max >= MAX_LUNS) max = MAX_LUNS - 1;
    kprintf("usbmsc: slot %u bulk-only storage, %u unit%s\n", d->slot, max + 1u, max ? "s" : "");
    for (unsigned lun = 0; lun <= max && m->dev; lun++) probe_lun(m, lun);
}

/* The block devices stay registered (a mounted filesystem may still hold them); they fail from
 * now on. */
void usbmsc_detach(struct usb_device *d) {
    struct msc *m = d->msc;
    if (!m) return;
    m->dev = NULL;
    d->msc = NULL;
}
