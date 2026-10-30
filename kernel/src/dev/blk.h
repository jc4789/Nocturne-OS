/* Block devices: every disk driver (ATA, NVMe, USB, Hyper-V SCSI) registers its disks here, and the
   filesystems scan them. Requests are in 512-byte sectors.

   Writing and persisting are separate requests. A write returns once the device has the data, which
   may still sit in its volatile cache; blk_persist returns once everything written before it is on
   the medium. A filesystem orders its writes with it (data before the record that names it) and asks
   for it once per save, not once per write.

   Any CPU, any task: a driver serializes the requests it gets for a device itself (its own lock,
   or the one thread that reaches it); blk.c keeps only the list of devices and the power gate. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct blkdev {
    int index;
    char name[16];  /* "ata0", "scsi1" */
    char model[41];
    uint64_t sectors;
    uint32_t block;  /* the device's own block in bytes (512 or 4096): a write covering whole blocks
                        needs no read-modify-write in the driver */
    uint32_t errors;    /* failed requests since the device appeared (atomic) */
    uint64_t written;   /* writes the device took, ever (atomic, blk.c) */
    uint64_t persisted; /* of them, the ones a persist that ended began after (atomic, blk.c) */
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    /* Everything written so far reaches the medium. NULL: the device keeps no volatile cache. */
    int (*persist)(struct blkdev *d);
    void *priv;
};

void blk_register(struct blkdev *d);
int blk_count(void);
struct blkdev *blk_get(int i);
int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf);
int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf);
int blk_persist(struct blkdev *d);
/* For a driver whose device moves blocks of d->block bytes: a request in 512-byte sectors as moves
   of whole blocks, at most `most` at a time. move(ctx, write, block, n, buf) moves n blocks between
   the device and buf: `bounce`, or the caller's memory itself where that holds whole blocks and
   `direct` is set (the device reaches any kernel memory). Blocks a write covers only partly are
   read into `bounce` first. Returns 0 or -N_ERR_IO. */
typedef int (*blk_move_fn)(void *ctx, bool write, uint64_t block, unsigned n, uint8_t *buf);
int blk_by_blocks(struct blkdev *d, bool write, uint64_t lba, uint32_t count, uint8_t *p, uint8_t *bounce,
                  unsigned most, bool direct, blk_move_fn move, void *ctx);
/* Power changes: blk_hold waits for every transfer under way to end, and from then on only the
   calling thread reaches the disks; everyone else waits in blk_read/blk_write until blk_release.
   Across a sleep no request is lost with the controller's state, and between a hibernation
   snapshot and the power going off nothing changes on disk that the snapshot does not know. */
void blk_hold(void);
void blk_release(void);
