/* 従来の IDE のポートの上の ATA の hard disk。PIO で、割り込みは使わず（nIEN）見て待つ。Hyper-V の第 1 世代の VM と
   QEMU の「pc」の機械にはこれで足りる。ATAPI（DVD）の drive は飛ばす。固定の節「ide-ports」の実装（vm/vm.c が節を
   出し、実装を足す）。ポートは節の資源から取る：channel c の命令の block（8 ポート）が slot 2c、制御の block が
   slot 2c+1。attach は資源のある channel ごとに master と slave を見て、見つけた drive ごとに block 装置にし、その
   要求は自分の thread が一つずつ行う（blk_serial_ops）。master と slave は channel の登録を分ける：channel の命令は
   setup から終わりまで channel の錠を持つ。

   電源。DMA も割り込みも無いので、止めるのは要求の道だけ：suspend から resume まで、来た要求は channel の錠の下で
   待つ。ディスクの揮発の cache の書き戻しは機械の段取り（dev/pm.c の flush_disks、blk_persist）が walk の前に行い、
   その後は blk_hold が要求を止めている。resume は drive が電源を失ったかを知らない（hal/hal.h）：各 drive を
   IDENTIFY で見直し（nIEN も立て直す）、同じ drive でなければ要求を失敗させ、揮発の cache を失ったかもしれないと
   block の層に言う（blk_cache_lost）。 */
#include "kernel.h"
#include "dev/blk.h"
#include "hal/hal.h"
#include "arch/cpu.h"
#include "mm/vmm.h"
#include "sys/wait.h"
#include "vm/ata.h"

/* 錠：channel の lock（mutex）の下で、asleep と、その channel の drive の lost と、登録。 */
struct ata_disk {
    struct blkdev blk;
    uint16_t io, ctl;
    uint8_t channel; /* channels の番号 */
    bool slave, lba48;
    bool lost; /* resume で同じ drive が見つからなかった：要求は失敗する */
    uint64_t sectors;
    char model[41];
};

struct ata_channel {
    struct mutex lock;       /* 登録：命令の setup から終わりまで持つ */
    struct wait_queue woken; /* asleep が戻るのを待つ要求 */
    bool asleep;             /* suspend から resume まで */
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

static struct ata_channel channels[2]; /* 節の channel ごとの登録 */
static struct ata_disk disks[2 * ARRAY_SIZE(channels)];
static int ndisks;

static struct ata_channel *channel_of(const struct ata_disk *d) { return &channels[d->channel]; }

/* 節の channel c のポート。命令の block と制御の block のどちらかの資源が無ければ false。 */
static bool channel_ports(const struct hal_device *d, unsigned c, uint16_t *io, uint16_t *ctl) {
    const struct n_hal_resource *command = hal_resource(d, N_HAL_PORT, 2 * c);
    const struct n_hal_resource *control = hal_resource(d, N_HAL_PORT, 2 * c + 1);
    if (!command || command->size < 8 || !control || !control->size) return false;
    *io = (uint16_t)command->base;
    *ctl = (uint16_t)control->base;
    return true;
}

/* DRQ の 512 バイトの sector をちょうど一つ運ぶ。256 個の一語の命令でなく、限りのある一つの文字列の I/O で。 */
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

/* 一か所の drive を IDENTIFY で見る。割り込みは切る（nIEN）。ATA の hard disk なら *out を埋めて true。attach と
   resume が呼ぶ（resume は channel の錠の下）。 */
static bool identify(uint16_t io, uint16_t ctl, bool slave, struct ata_disk *out) {
    outb(ctl, 0x02); /* nIEN：割り込みなし、見て待つ */
    outb(io + REG_DRIVE, slave ? 0xB0 : 0xA0);
    delay400(ctl);
    if (inb(io + REG_STATUS) == 0xFF) return false; /* 浮いたバス：制御器が無い */
    outb(io + REG_COUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, 0xEC); /* IDENTIFY */
    delay400(ctl);
    if (inb(io + REG_STATUS) == 0) return false; /* drive が無い */
    struct ata_disk d = {.io = io, .ctl = ctl, .slave = slave};
    if (wait_not_busy(&d) < 0) return false;
    if (inb(io + REG_LBA1) || inb(io + REG_LBA2)) return false; /* ATAPI か SATA の印 */
    if (wait_drq(&d) < 0) return false;
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
    if (!d.sectors) return false;
    *out = d;
    return true;
}

static void probe(unsigned channel, uint16_t io, uint16_t ctl, bool slave) {
    struct ata_disk d;
    if (!identify(io, ctl, slave, &d)) return;
    d.channel = (uint8_t)channel;
    disks[ndisks++] = d;
    kprintf("ata: channel %u %s: %s, %lu MiB%s\n", channel, slave ? "slave" : "master", d.model, d.sectors / 2048,
            d.lba48 ? ", LBA48" : "");
}

static int transfer(struct blkdev *b, const struct blk_request *r);

static void disk_register(struct hal_device *hal, int i) {
    struct blkdev *b = &disks[i].blk;
    ksnprintf(b->name, sizeof b->name, "ata%d", i);
    strlcpy(b->model, disks[i].model, sizeof b->model);
    b->sectors = disks[i].sectors;
    b->max_sectors = 128; /* READ・WRITE SECTORS の命令一つ：数は 1 バイトで 0 が 256（48 bit なら 16 bit） */
    b->has_cache = true;
    b->priv = &disks[i];
    blk_serial_start(b, transfer);
    blk_register(hal, b);
}

static int ata_attach(struct hal_device *d) {
    if (ndisks) return -N_ERR_EXISTS;
    for (unsigned c = 0; c < ARRAY_SIZE(channels); c++) {
        uint16_t io, ctl;
        if (!channel_ports(d, c, &io, &ctl)) continue;
        hal_enable(d, HAL_ENABLE_PORTS);
        probe(c, io, ctl, false);
        probe(c, io, ctl, true);
    }
    if (!ndisks) return HAL_DECLINE;
    for (int i = 0; i < ndisks; i++) disk_register(d, i);
    return 0;
}

/* 進行中の命令の終わりを待ち、その後の要求を resume まで待たせる。三つの目標で同じ。 */
static bool ata_suspend(struct hal_device *d, enum hal_power_target target) {
    (void)d;
    (void)target;
    for (unsigned c = 0; c < ARRAY_SIZE(channels); c++) {
        mutex_lock(&channels[c].lock);
        channels[c].asleep = true;
        mutex_unlock(&channels[c].lock);
    }
    return true;
}

/* channel の錠の下：disk の drive が attach の時と同じか。 */
static bool same_drive(const struct ata_disk *disk) {
    struct ata_disk now;
    return identify(disk->io, disk->ctl, disk->slave, &now) && now.sectors == disk->sectors &&
           now.lba48 == disk->lba48 && !strcmp(now.model, disk->model);
}

static bool ata_resume(struct hal_device *d) {
    bool all = true;
    for (unsigned c = 0; c < ARRAY_SIZE(channels); c++) {
        mutex_lock(&channels[c].lock);
        for (int i = 0; i < ndisks; i++) {
            struct ata_disk *disk = &disks[i];
            if (channel_of(disk) != &channels[c] || disk->lost) continue;
            if (!same_drive(disk)) {
                disk->lost = true;
                all = false;
                kprintf("ata: %s is not the drive it was before the sleep\n", disk->blk.name);
            }
            blk_cache_lost(&disk->blk);
        }
        channels[c].asleep = false;
        mutex_unlock(&channels[c].lock);
        wq_wake_all(&channels[c].woken);
    }
    if (!all) hal_failed(d, -N_ERR_IO);
    return all;
}

static const struct n_hal_rule ata_rules[] = {N_FIXED("ide-ports")};
const struct hal_driver ata_driver = {
    .name = "ata",
    .rules = ata_rules,
    .rule_count = ARRAY_SIZE(ata_rules),
    .attach = ata_attach,
    .suspend = ata_suspend,
    .resume = ata_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};

static int setup(struct ata_disk *d, uint64_t lba, uint32_t count, bool write) {
    if (lba + count > d->sectors) return -N_ERR_IO;
    if (wait_not_busy(d) < 0) return -N_ERR_IO;
    if (d->lba48) {
        outb(d->io + REG_DRIVE, 0x40 | (d->slave ? 0x10 : 0));
        delay400(d->ctl);
        if (wait_not_busy(d) < 0) return -N_ERR_IO; /* 新しく選んだ drive も ready であること */
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
        if (wait_not_busy(d) < 0) return -N_ERR_IO; /* 新しく選んだ drive も ready であること */
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + REG_COMMAND, write ? 0x30 : 0x20);
    }
    return 0;
}

/* 要求の READ SECTORS (EXT) 一つ。channel の錠の下。各 sector は一つの区分の中にある（区分のバイトは 512 の倍数）。 */
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

/* 要求の WRITE SECTORS (EXT) 一つ。channel の錠の下。 */
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

/* FLUSH CACHE (EXT)：drive の書込みの cache を媒体へ（ATA8-ACS 7.10、7.11）。 */
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

/* 書込みの命令は block の層に数える（blk_write_given・blk_write_ended）：resume の blk_cache_lost が、どの persist にも
   覆われない書込みを知る。 */
static int transfer(struct blkdev *b, const struct blk_request *r) {
    struct ata_disk *d = b->priv;
    struct ata_channel *ch = channel_of(d);
    mutex_lock(&ch->lock);
    while (ch->asleep) mutex_wait(&ch->lock, &ch->woken);
    int s = -N_ERR_IO;
    if (!d->lost) {
        switch (r->op) {
        case BLK_READ: s = read_command(d, r); break;
        case BLK_WRITE:
            blk_write_given(b);
            s = write_command(d, r);
            blk_write_ended(b);
            break;
        case BLK_PERSIST: s = flush_command(d); break;
        }
    }
    mutex_unlock(&ch->lock);
    return s;
}
