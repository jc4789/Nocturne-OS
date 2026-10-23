#include "kernel.h"
#include "arch/cpu.h"
#include "dev/blk.h"
#include "sys/sched.h"

#define MAX_BLK 16
static struct blkdev *devs[MAX_BLK];
static int ndevs;

void blk_register(struct blkdev *d) {
    if (ndevs == MAX_BLK) return;
    if (!d->block) d->block = 512;
    d->index = ndevs;
    devs[ndevs++] = d;
    kprintf("blk: %s: %s, %lu MiB, %u-byte blocks%s\n", d->name, d->model, d->sectors / 2048, d->block,
            d->persist ? "" : ", no write cache");
}

int blk_count(void) { return ndevs; }

struct blkdev *blk_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }

static struct wait_queue gate_wq;
static struct task *holder;
static unsigned inflight;

static void enter(void) {
    uint64_t f = irq_save();
    while (holder && holder != current_task) wq_wait(&gate_wq);
    inflight++;
    irq_restore(f);
}

static void leave(void) {
    uint64_t f = irq_save();
    if (!--inflight && holder) wq_wake_all(&gate_wq);
    irq_restore(f);
}

/* A failed request is counted, and the first few are logged: a dying disk must not flood the log.
   Drivers fail with -1 or a code of their own; callers see N_ERR_IO, or N_ERR_TIMEOUT. */
static int failed(struct blkdev *d, const char *what, uint64_t lba, uint32_t n, int r) {
    if (d->errors++ < 8) kprintf("blk: %s: %s of %u sectors at %lu failed (%d)\n", d->name, what, n, lba, r);
    return r == -N_ERR_TIMEOUT ? r : -N_ERR_IO;
}

int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    enter();
    int r = d->read(d, lba, n, buf);
    leave();
    return r ? failed(d, "read", lba, n, r) : 0;
}

/* A write is marked once the device has it, and a persist clears the mark before it flushes: what
   was written before a persist began is in its flush, and what lands during it is in the next. */
int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    enter();
    int r = d->write(d, lba, n, buf);
    d->unpersisted = true;
    leave();
    return r ? failed(d, "write", lba, n, r) : 0;
}

int blk_by_blocks(struct blkdev *d, bool write, uint64_t lba, uint32_t count, uint8_t *p, uint8_t *bounce,
                  unsigned most, bool direct, blk_move_fn move, void *ctx) {
    if (lba + count > d->sectors) return -N_ERR_IO;
    unsigned per = d->block / 512;
    while (count) {
        uint64_t first = lba / per;
        unsigned off = (unsigned)(lba % per) * 512, want = count * 512u;
        unsigned n = (off + want + d->block - 1) / d->block;
        if (n > most) n = most;
        unsigned take = n * d->block - off;
        if (take > want) take = want;
        bool whole = !off && take == n * d->block;
        if (whole && direct) {
            if (move(ctx, write, first, n, p) < 0) return -N_ERR_IO;
        } else if (write) {
            if (!whole && move(ctx, false, first, n, bounce) < 0) return -N_ERR_IO;
            memcpy(bounce + off, p, take);
            if (move(ctx, true, first, n, bounce) < 0) return -N_ERR_IO;
        } else {
            if (move(ctx, false, first, n, bounce) < 0) return -N_ERR_IO;
            memcpy(p, bounce + off, take);
        }
        lba += take / 512;
        count -= take / 512;
        p += take;
    }
    return 0;
}

/* Nothing written since the last persist: the medium has it all already. */
int blk_persist(struct blkdev *d) {
    if (!d->persist) return 0;
    enter();
    int r = 0;
    if (d->unpersisted) {
        d->unpersisted = false;
        if ((r = d->persist(d))) d->unpersisted = true;
    }
    leave();
    return r ? failed(d, "cache flush", 0, 0, r) : 0;
}

void blk_hold(void) {
    uint64_t f = irq_save();
    holder = current_task;
    while (inflight) wq_wait(&gate_wq);
    irq_restore(f);
}

void blk_release(void) {
    uint64_t f = irq_save();
    holder = NULL;
    wq_wake_all(&gate_wq);
    irq_restore(f);
}
