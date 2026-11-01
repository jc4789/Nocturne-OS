/* Block devices: every disk driver (ATA, NVMe, USB, Hyper-V SCSI) registers its disks here, and the
   filesystems scan them. Requests are in 512-byte sectors.

   Writing and persisting are separate requests. A write returns once the device has the data, which
   may still sit in its volatile cache; blk_persist returns once everything written before it is on
   the medium. A filesystem orders its writes with it (data before the record that names it) and asks
   for it once per save, not once per write.

   Any CPU, any task: a driver orders the requests it gets for a device itself, one at a time or
   side by side as the device takes them; blk.c keeps the list of devices and the power gate, and
   writes of part of a device block one at a time (blk_by_blocks). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sys/wait.h"

struct blkdev {
    int index;
    char name[16];  /* "ata0", "scsi1" */
    char model[41];
    uint64_t sectors;
    uint32_t block;  /* the device's own block in bytes, a power of two from 512: a write covering
                        whole blocks needs no read-modify-write in the driver */
    bool gone;       /* atomic: the device left (unplugged, its adapter dead). The device stays listed
                        and its requests fail; a power change does not wait for what it cannot keep */
    uint32_t errors;    /* failed requests since the device appeared (atomic) */
    uint64_t written;   /* requests to write that ended, ever (atomic, blk.c) */
    uint64_t persisted; /* of them, the ones a persist that ended began after (atomic, blk.c) */
    /* A driver that calls blk_cache_lost counts its write commands, all atomic: */
    uint64_t given;     /* those that went into the device (blk_write_given) */
    uint64_t ended;     /* of them, the ones that are over, however (blk_write_ended) */
    uint64_t flushed;   /* of them, the ones a persist that ended began after (blk.c) */
    bool cache_lost;    /* blk_cache_lost: for good, every persist fails (atomic) */
    struct mutex partial; /* parts of a block written at once: each one's read, change and write
                             whole, so none writes back another's bytes as they were */
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    /* Everything written so far reaches the medium. NULL: the device never keeps a volatile cache;
       a device whose cache may come and go has one that does nothing while it is off. */
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
/* The thread blk_hold let through, NULL when there is none. */
struct thread *blk_holder(void);
/* The device may have started over (it lost its link and maybe its power with it, it stopped
   answering, its controller failed), with no document saying that its volatile cache outlives that:
   what that held may be gone, which a flush of what is left cannot tell. If a write command went
   into it that no blk_persist covers, every blk_persist fails from here on, once it has flushed, so
   that whoever wrote that finds out, whichever of them comes first. A volume there goes no further,
   and the medium keeps the last commit that was whole. On a device with no persist this changes
   nothing; a driver that knows the cache to be off at the time does not call this. A flush that has
   ended but is not counted yet counts as not done: every persist fails then as well, and nothing is lost. */
void blk_cache_lost(struct blkdev *d);
/* The driver's count of its write commands: before the device can see one, and once that one is
   over, whether it took it or not, or nothing of it moves any more. Each try of a request counts.
   Any CPU, under a spinlock too. */
void blk_write_given(struct blkdev *d);
void blk_write_ended(struct blkdev *d);
/* A driver's own thread at work on its disks counts as a transfer under way: blk_hold waits for it,
   and it waits for blk_release. */
void blk_busy_begin(void);
void blk_busy_end(void);
