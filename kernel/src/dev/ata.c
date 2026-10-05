/* ATA hard disks on the legacy IDE ports, PIO mode with polling (interrupts off via nIEN).
   That is all a Hyper-V Generation 1 VM or QEMU's "pc" machine needs. ATAPI (DVD) drives are
   skipped. The kernel is non-preemptive, so a transfer simply runs to completion. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/ata.h"

#define REG_DATA    0
#define REG_ERROR   1
#define REG_COUNT   2
#define REG_LBA0    3
#define REG_LBA1    4
#define REG_LBA2    5
#define REG_DRIVE   6
#define REG_STATUS  7
#define REG_COMMAND 7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static struct ata_disk disks[4];
static int ndisks;

static void delay400(uint16_t ctl) {
    for (int i = 0; i < 4; i++) inb(ctl);
}

static int wait_not_busy(struct ata_disk *d) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(d->io + REG_STATUS);
        if (!(s & ST_BSY)) return s;
        pause();
    }
    return -1;
}

static int wait_drq(struct ata_disk *d) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(d->io + REG_STATUS);
        if (s & ST_BSY) {
            pause();
            continue;
        }
        if (s & (ST_ERR | ST_DF)) return -EIO;
        if (s & ST_DRQ) return 0;
        pause();
    }
    return -EIO;
}

static void probe(uint16_t io, uint16_t ctl, bool slave) {
    outb(ctl, 0x02); /* nIEN: no interrupts, we poll */
    outb(io + REG_DRIVE, slave ? 0xB0 : 0xA0);
    delay400(ctl);
    if (inb(io + REG_STATUS) == 0xFF) return; /* floating bus: no controller */
    outb(io + REG_COUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, 0xEC); /* IDENTIFY */
    delay400(ctl);
    if (inb(io + REG_STATUS) == 0) return; /* no drive */
    struct ata_disk d = {.io = io, .ctl = ctl, .slave = slave};
    if (wait_not_busy(&d) < 0) return;
    if (inb(io + REG_LBA1) || inb(io + REG_LBA2)) return; /* ATAPI or SATA signature */
    if (wait_drq(&d) < 0) return;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io + REG_DATA);
    d.lba48 = id[83] & (1u << 10);
    d.sectors = d.lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32
                        : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    for (int i = 0; i < 20; i++) {
        d.model[i * 2] = (char)(id[27 + i] >> 8);
        d.model[i * 2 + 1] = (char)id[27 + i];
    }
    for (int i = 39; i >= 0 && d.model[i] == ' '; i--) d.model[i] = 0;
    if (!d.sectors) return;
    d.index = ndisks;
    disks[ndisks++] = d;
    kprintf("ata: %s %s: %s, %lu MiB%s\n", io == 0x1F0 ? "primary" : "secondary", slave ? "slave" : "master",
            d.model, d.sectors / 2048, d.lba48 ? ", LBA48" : "");
}

void ata_init(void) {
    probe(0x1F0, 0x3F6, false);
    probe(0x1F0, 0x3F6, true);
    probe(0x170, 0x376, false);
    probe(0x170, 0x376, true);
}

int ata_count(void) { return ndisks; }
struct ata_disk *ata_get(int i) { return i >= 0 && i < ndisks ? &disks[i] : NULL; }

static int setup(struct ata_disk *d, uint64_t lba, uint32_t count, bool write) {
    if (lba + count > d->sectors) return -EIO;
    if (wait_not_busy(d) < 0) return -EIO;
    if (d->lba48) {
        outb(d->io + REG_DRIVE, 0x40 | (d->slave ? 0x10 : 0));
        delay400(d->ctl);
        if (wait_not_busy(d) < 0) return -EIO; /* the newly selected drive must be ready too */
        outb(d->io + REG_COUNT, (uint8_t)(count >> 8));
        outb(d->io + REG_LBA0, (uint8_t)(lba >> 24));
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 32));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 40));
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + REG_COMMAND, write ? 0x34 : 0x24);
    } else {
        if (lba >> 28) return -EIO;
        outb(d->io + REG_DRIVE, 0xE0 | (d->slave ? 0x10 : 0) | (uint8_t)(lba >> 24));
        delay400(d->ctl);
        if (wait_not_busy(d) < 0) return -EIO; /* the newly selected drive must be ready too */
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + REG_COMMAND, write ? 0x30 : 0x20);
    }
    return 0;
}

int ata_read(struct ata_disk *d, uint64_t lba, uint32_t count, void *buf) {
    uint16_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        int r = setup(d, lba, n, false);
        if (r < 0) return r;
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq(d) < 0) return -EIO;
            for (int i = 0; i < 256; i++) *p++ = inw(d->io + REG_DATA);
            delay400(d->ctl);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(struct ata_disk *d, uint64_t lba, uint32_t count, const void *buf) {
    const uint16_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        int r = setup(d, lba, n, true);
        if (r < 0) return r;
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq(d) < 0) return -EIO;
            for (int i = 0; i < 256; i++) outw(d->io + REG_DATA, *p++);
            delay400(d->ctl);
        }
        lba += n;
        count -= n;
    }
    /* make sure it reaches the disk image before we report success */
    if (wait_not_busy(d) < 0) return -EIO;
    outb(d->io + REG_COMMAND, d->lba48 ? 0xEA : 0xE7);
    if (wait_not_busy(d) < 0) return -EIO;
    return 0;
}
