/* ATA hard disks on the legacy IDE ports, PIO mode with polling (interrupts off via nIEN).
   That is all a Hyper-V Generation 1 VM or QEMU's "pc" machine needs. ATAPI (DVD) drives are
   skipped. The driver of the fixed node "ide-ports" (vm/vm.c publishes it and adds the driver):
   attach looks at the four places a drive may be, and each drive found is a block device whose
   requests one thread of its own does one at a time (blk_serial_ops). A master and its slave share
   their channel's registers: a command on a channel holds the channel's lock from its setup to its
   end. */
#include "kernel.h"
#include "dev/blk.h"
#include "hal/hal.h"
#include "arch/cpu.h"
#include "mm/vmm.h"
#include "sys/wait.h"
#include "vm/ata.h"

struct ata_disk {
    struct blkdev blk;
    uint16_t io, ctl;
    bool slave, lba48;
    uint64_t sectors;
    char model[41];
};

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
static struct mutex channels[2]; /* the primary's (0x1F0) and the secondary's registers */

static struct mutex *channel_of(const struct ata_disk *d) { return &channels[d->io == 0x1F0 ? 0 : 1]; }

/* Transfer exactly one 512-byte DRQ sector. Use one bounded string I/O
   operation instead of 256 scalar instructions; device
   command/status/DRQ and cache-flush ordering remain unchanged. */
static void pio_read_sector(uint16_t port, void *buffer) {
    size_t words = 256;
    __asm__ volatile("cld; rep insw" : "+D"(buffer), "+c"(words) : "d"(port) : "memory", "cc");
}
static void pio_write_sector(uint16_t port, const void *buffer) {
    size_t words = 256;
    __asm__ volatile("cld; rep outsw" : "+S"(buffer), "+c"(words) : "d"(port) : "memory", "cc");
}

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
        if (s & (ST_ERR | ST_DF)) return -N_ERR_IO;
        if (s & ST_DRQ) return 0;
        pause();
    }
    return -N_ERR_IO;
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
    pio_read_sector(io + REG_DATA, id);
    d.lba48 = id[83] & (1u << 10);
    d.sectors = d.lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32
                        : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    for (int i = 0; i < 20; i++) {
        d.model[i * 2] = (char)(id[27 + i] >> 8);
        d.model[i * 2 + 1] = (char)id[27 + i];
    }
    for (int i = 39; i >= 0 && d.model[i] == ' '; i--) d.model[i] = 0;
    if (!d.sectors) return;
    disks[ndisks++] = d;
    kprintf("ata: %s %s: %s, %lu MiB%s\n", io == 0x1F0 ? "primary" : "secondary", slave ? "slave" : "master",
            d.model, d.sectors / 2048, d.lba48 ? ", LBA48" : "");
}

static int transfer(struct blkdev *b, const struct blk_request *r);

static void disk_register(struct hal_device *hal, int i) {
    struct blkdev *b = &disks[i].blk;
    ksnprintf(b->name, sizeof b->name, "ata%d", i);
    strlcpy(b->model, disks[i].model, sizeof b->model);
    b->sectors = disks[i].sectors;
    b->max_sectors = 128; /* one READ or WRITE SECTORS command: the count is a byte, 0 meaning 256 (48-bit: 16 bits) */
    b->has_cache = true;
    b->priv = &disks[i];
    blk_serial_start(b, transfer);
    blk_register(hal, b);
}

static int ata_attach(struct hal_device *d) {
    if (ndisks) return -N_ERR_EXISTS;
    probe(0x1F0, 0x3F6, false);
    probe(0x1F0, 0x3F6, true);
    probe(0x170, 0x376, false);
    probe(0x170, 0x376, true);
    if (!ndisks) return HAL_DECLINE;
    for (int i = 0; i < ndisks; i++) disk_register(d, i);
    return 0;
}

/* The ports keep what they had across a sleep: nothing is owed to the drives (a request that comes
   waits for the channel as before). */
static bool ata_resume(struct hal_device *d) {
    (void)d;
    return true;
}

static const struct n_hal_rule ata_rules[] = {N_FIXED("ide-ports")};
const struct hal_driver ata_driver = {
    .name = "ata",
    .rules = ata_rules,
    .rule_count = ARRAY_SIZE(ata_rules),
    .attach = ata_attach,
    .resume = ata_resume,
};

static int setup(struct ata_disk *d, uint64_t lba, uint32_t count, bool write) {
    if (lba + count > d->sectors) return -N_ERR_IO;
    if (wait_not_busy(d) < 0) return -N_ERR_IO;
    if (d->lba48) {
        outb(d->io + REG_DRIVE, 0x40 | (d->slave ? 0x10 : 0));
        delay400(d->ctl);
        if (wait_not_busy(d) < 0) return -N_ERR_IO; /* the newly selected drive must be ready too */
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
        if (lba >> 28) return -N_ERR_IO;
        outb(d->io + REG_DRIVE, 0xE0 | (d->slave ? 0x10 : 0) | (uint8_t)(lba >> 24));
        delay400(d->ctl);
        if (wait_not_busy(d) < 0) return -N_ERR_IO; /* the newly selected drive must be ready too */
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + REG_COMMAND, write ? 0x30 : 0x20);
    }
    return 0;
}

/* One READ SECTORS (EXT) of the request, the channel's lock held. Each sector lies within one
   segment (their bytes are multiples of 512). */
static int read_command(struct ata_disk *d, const struct blk_request *r) {
    int s = setup(d, r->lba, r->sectors, false);
    if (s < 0) return s;
    for (uint32_t i = 0; i < r->segment_count; i++) {
        uint16_t *p = phys_to_virt(r->segments[i].phys);
        for (uint32_t k = 0; k < r->segments[i].bytes / 512; k++, p += 256) {
            if (wait_drq(d) < 0) return -N_ERR_IO;
            pio_read_sector(d->io + REG_DATA, p);
            delay400(d->ctl);
        }
    }
    return 0;
}

/* One WRITE SECTORS (EXT) of the request, the channel's lock held. */
static int write_command(struct ata_disk *d, const struct blk_request *r) {
    int s = setup(d, r->lba, r->sectors, true);
    if (s < 0) return s;
    for (uint32_t i = 0; i < r->segment_count; i++) {
        const uint16_t *p = phys_to_virt(r->segments[i].phys);
        for (uint32_t k = 0; k < r->segments[i].bytes / 512; k++, p += 256) {
            if (wait_drq(d) < 0) return -N_ERR_IO;
            pio_write_sector(d->io + REG_DATA, p);
            delay400(d->ctl);
        }
    }
    return wait_not_busy(d) < 0 ? -N_ERR_IO : 0;
}

/* FLUSH CACHE (EXT): the drive's write cache reaches the medium (ATA8-ACS 7.10, 7.11). */
static int flush_command(struct ata_disk *d) {
    int s = wait_not_busy(d);
    if (s >= 0) {
        outb(d->io + REG_DRIVE, 0xA0 | (d->slave ? 0x10 : 0));
        delay400(d->ctl);
        s = wait_not_busy(d);
    }
    if (s >= 0) {
        outb(d->io + REG_COMMAND, d->lba48 ? 0xEA : 0xE7);
        delay400(d->ctl);
        s = wait_not_busy(d);
    }
    return s < 0 || s & (ST_ERR | ST_DF) ? -N_ERR_IO : 0;
}

static int transfer(struct blkdev *b, const struct blk_request *r) {
    struct ata_disk *d = b->priv;
    mutex_lock(channel_of(d));
    int s = r->op == BLK_READ ? read_command(d, r) : r->op == BLK_WRITE ? write_command(d, r) : flush_command(d);
    mutex_unlock(channel_of(d));
    return s;
}
