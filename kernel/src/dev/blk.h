/* Block devices: every disk driver (ATA, Hyper-V SCSI) registers its disks here, and the FAT
   filesystem scans them. Sectors are 512 bytes. */
#pragma once
#include <stdint.h>

struct blkdev {
    int index;
    char name[16];  /* "ata0", "scsi1" */
    char model[41];
    uint64_t sectors;
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
};

void blk_register(struct blkdev *d);
int blk_count(void);
struct blkdev *blk_get(int i);
static inline int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) { return d->read(d, lba, n, buf); }
static inline int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) { return d->write(d, lba, n, buf); }
