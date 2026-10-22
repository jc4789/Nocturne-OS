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

int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    enter();
    int r = d->write(d, lba, n, buf);
    leave();
    return r ? failed(d, "write", lba, n, r) : 0;
}

int blk_persist(struct blkdev *d) {
    if (!d->persist) return 0;
    enter();
    int r = d->persist(d);
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
