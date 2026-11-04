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
    char name[24];  /* for the log: "nvme0.1" (controller, namespace), "sata0.2" (adapter, port), "usb0" */
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
    /* What the driver declares before blk_register. A request it gets stays within these: blk.c
       splits what is larger, and turns a write of part of a block into its read, change and write,
       so a driver only ever moves whole blocks. */
    uint32_t max_sectors;    /* in one request; 0: no limit */
    uint32_t max_segments;   /* in one request; 0: no limit */
    uint32_t segment_align;  /* a segment's physical address and length are multiples of this; 0: 512 */
    bool has_cache;          /* the device may keep a volatile cache: BLK_PERSIST reaches it */
    const struct blk_ops *ops;
    void *priv;
};

/* A request: chained through next into a batch, so one submit can fill a device's queue with one
   doorbell. A segment is physical memory; blk.c gives the driver bus-ready lists, and no request
   goes through a copy here. done runs exactly once per request, in any context (an interrupt
   handler too), never sleeps, and status is 0 or a negative N_ERR_*. BLK_PERSIST moves no data:
   everything whose done ran before it was submitted is on the medium when its done runs. */
enum blk_op { BLK_READ, BLK_WRITE, BLK_PERSIST };
struct blk_segment {
    uint64_t phys;
    uint32_t bytes;
};
struct blk_request {
    struct blk_request *next;
    enum blk_op op;
    uint64_t lba;
    uint32_t sectors;
    const struct blk_segment *segments;
    uint32_t segment_count;
    void (*done)(struct blk_request *r, int status);
    void *context;
    struct blkdev *device; /* blk.c's */
    void *owned;           /* blk.c's */
};
/* The data path: called directly by blk.c from a thread that may sleep, any CPU, several at once on
   one device. submit starts all of the chain and returns: it may wait for room in a queue, never
   for a request to end. A driver with queues per CPU uses the calling CPU's. */
struct blk_ops {
    void (*submit)(struct blkdev *d, struct blk_request *chain);
};
/* Submit a chain from any thread that may sleep, the power gate waiting at blk_hold. The way for
   anything that has more than one request to give (the block cache's read-ahead and write-back). */
void blk_submit(struct blkdev *d, struct blk_request *chain);
/* A device that does one thing at a time (a port's PIO, a USB bulk pipe): its driver gives transfer,
   and sets blk_serial_ops as ops. blk.c runs the requests one by one on a thread of that device's. */
typedef int (*blk_transfer_fn)(struct blkdev *d, const struct blk_request *r);
extern const struct blk_ops blk_serial_ops;
void blk_serial_start(struct blkdev *d, blk_transfer_fn transfer);

void blk_register(struct blkdev *d);
int blk_count(void);
struct blkdev *blk_get(int i);
int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf);
int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf);
int blk_persist(struct blkdev *d);
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
